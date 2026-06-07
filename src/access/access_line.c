/**
 * @file   access_line.c
 * @brief  ACCESS 线池与终端交互实现（行编辑/ANSI/历史/pager）
 * @author jhb
 * @date   2026/05/30
 *
 * 终端逻辑迁自原 CLI 的 cli_handler.c。输出直接写本线 fd（ACCESS 持有 socket），
 * 提示符使用 CLI 下发的已渲染字符串。命令执行/Tab/Help 通过向 CLI 发 RPC 完成
 * （见 access_cli.c），本文件只负责终端态与本地交互。
 */
#include "access_line.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "access_db.h"
#include "access_main.h"

// ============================================================================
// 线池（epoll 线程 alloc/free/process；IPC worker 线程按 line_id 写输出，故加锁）
// ============================================================================

static access_line_t g_lines[ACCESS_LINE_POOL_SIZE];
static access_global_history_t g_global_history;
static GMutex g_pool_mutex;

/** vty 每线 transport input 位掩码，索引 = vty_num(0..ACCESS_VTY_COUNT-1)，默认 0=none */
static uint8_t g_vty_transport[ACCESS_VTY_COUNT];

/** 全局 telnet server 开关：1=监听 23（`telnet server enable`）。默认 0=出厂只 console。
 *  与 per-line transport input 是两道独立闸门：监听靠它，能否接入靠 per-line。 */
static int g_telnet_server_enabled = 0;

int access_telnet_server_enabled(void)
{
    return g_telnet_server_enabled;
}

void access_set_telnet_server_enabled(int enabled)
{
    g_telnet_server_enabled = enabled ? 1 : 0;
}

void access_vty_set_transport(uint32_t first, uint32_t last, uint8_t bits)
{
    if (first > last)
    {
        uint32_t t = first;
        first = last;
        last = t;
    }
    if (last >= ACCESS_VTY_COUNT)
    {
        last = ACCESS_VTY_COUNT - 1;
    }
    for (uint32_t i = first; i <= last && i < ACCESS_VTY_COUNT; i++)
    {
        g_vty_transport[i] = bits; /* replace 语义，与 Cisco transport input 一致 */
    }
}

int access_vty_any_telnet(void)
{
    for (uint32_t i = 0; i < ACCESS_VTY_COUNT; i++)
    {
        if (g_vty_transport[i] & ACCESS_TRANSPORT_TELNET)
        {
            return 1;
        }
    }
    return 0;
}

/** 某条 vty 线（line_id）是否允许 telnet 接入 */
static int vty_line_allows_telnet(uint32_t line_id)
{
    if (line_id < ACCESS_VTY_FIRST_ID || line_id >= ACCESS_LINE_POOL_SIZE)
    {
        return 0;
    }
    uint32_t vty_num = line_id - ACCESS_VTY_FIRST_ID;
    return (g_vty_transport[vty_num] & ACCESS_TRANSPORT_TELNET) ? 1 : 0;
}

void access_line_pool_init(void)
{
    g_mutex_init(&g_pool_mutex);
    memset(g_lines, 0, sizeof(g_lines));
    for (uint32_t i = 0; i < ACCESS_LINE_POOL_SIZE; i++)
    {
        g_lines[i].ep_kind = ACCESS_EP_LINE;
        g_lines[i].line_id = i;
        g_lines[i].fd = -1;
    }
    access_global_history_init(&g_global_history);
}

void access_line_pool_cleanup(void)
{
    g_mutex_lock(&g_pool_mutex);
    for (uint32_t i = 0; i < ACCESS_LINE_POOL_SIZE; i++)
    {
        if (g_lines[i].in_use)
        {
            if (g_lines[i].fd >= 0)
            {
                close(g_lines[i].fd);
                g_lines[i].fd = -1;
            }
            access_session_history_cleanup(&g_lines[i].history);
            if (g_lines[i].pager_buffer)
            {
                g_free(g_lines[i].pager_buffer);
                g_lines[i].pager_buffer = NULL;
            }
            g_lines[i].in_use = 0;
        }
    }
    g_mutex_unlock(&g_pool_mutex);
    access_global_history_cleanup(&g_global_history);
    g_mutex_clear(&g_pool_mutex);
}

access_line_t *access_line_alloc(int fd, const char *ip, uint16_t port, uint16_t line_type)
{
    g_mutex_lock(&g_pool_mutex);
    access_line_t *line = NULL;
    if (line_type == ACCESS_LINE_TYPE_CON)
    {
        /* console 专用槽（线 0），永远为串口保留，不参与 vty 竞争 */
        if (!g_lines[ACCESS_CON_LINE_ID].in_use)
        {
            line = &g_lines[ACCESS_CON_LINE_ID];
        }
    }
    else
    {
        /* vty 线 1..N-1：只分配 transport input 允许 telnet 的空闲线（per-line 门控） */
        for (uint32_t i = ACCESS_VTY_FIRST_ID; i < ACCESS_LINE_POOL_SIZE; i++)
        {
            if (!g_lines[i].in_use && vty_line_allows_telnet(i))
            {
                line = &g_lines[i];
                break;
            }
        }
    }
    if (!line)
    {
        g_mutex_unlock(&g_pool_mutex);
        return NULL; /* 对应池满 */
    }

    uint32_t saved_id = line->line_id;
    memset(line, 0, sizeof(*line));
    line->ep_kind = ACCESS_EP_LINE;
    line->line_id = saved_id;
    line->in_use = 1;
    line->fd = fd;
    line->line_type = line_type;
    line->state = ACCESS_INPUT_NORMAL;
    line->pager_lines_per_page = ACCESS_PAGER_DEFAULT_LINES;
    line->connect_time = time(NULL);
    line->client_port = port;
    g_strlcpy(line->client_ip, ip ? ip : "unknown", sizeof(line->client_ip));
    /* 初始提示符：建会话后由 CLI 下发覆盖（P3）。 */
    g_strlcpy(line->prompt, "<NetNexus>", sizeof(line->prompt));
    access_session_history_init(&line->history);

    g_mutex_unlock(&g_pool_mutex);
    return line;
}

void access_line_free(access_line_t *line)
{
    if (!line)
    {
        return;
    }
    g_mutex_lock(&g_pool_mutex);
    if (line->fd >= 0)
    {
        close(line->fd);
        line->fd = -1;
    }
    access_session_history_cleanup(&line->history);
    if (line->pager_buffer)
    {
        g_free(line->pager_buffer);
        line->pager_buffer = NULL;
    }
    line->in_use = 0;
    g_mutex_unlock(&g_pool_mutex);
}

access_line_t *access_line_find(uint32_t line_id)
{
    if (line_id >= ACCESS_LINE_POOL_SIZE)
    {
        return NULL;
    }
    access_line_t *line = &g_lines[line_id];
    return line->in_use ? line : NULL;
}

// ============================================================================
// 输出
// ============================================================================

static void access_write_best_effort(int fd, const void *data, size_t len)
{
    if (fd < 0 || !data || len == 0)
    {
        return;
    }
    const char *buf = (const char *)data;
    while (len > 0)
    {
        ssize_t n = write(fd, buf, len);
        if (n > 0)
        {
            buf += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct pollfd pfd = {
                .fd = fd,
                .events = POLLOUT,
                .revents = 0,
            };
            int rc;
            do
            {
                rc = poll(&pfd, 1, 1000);
            } while (rc < 0 && errno == EINTR);

            if (rc > 0 && (pfd.revents & POLLOUT))
            {
                continue;
            }
        }
        break;
    }
}

void access_line_send(access_line_t *line, const char *msg)
{
    if (line && msg)
    {
        access_write_best_effort(line->fd, msg, strlen(msg));
    }
}

void access_line_send_to(uint32_t line_id, const char *msg)
{
    if (!msg || line_id >= ACCESS_LINE_POOL_SIZE)
    {
        return;
    }

    g_mutex_lock(&g_pool_mutex);
    access_line_t *line = &g_lines[line_id];
    if (line->in_use)
    {
        access_line_send(line, msg);
    }
    g_mutex_unlock(&g_pool_mutex);
}

void access_line_send_data(access_line_t *line, const void *data, size_t len)
{
    if (line && data && len > 0)
    {
        access_write_best_effort(line->fd, data, len);
    }
}

/** 发送当前提示符（CLI 已渲染，直接输出 + 尾随空格） */
static void send_prompt(access_line_t *line)
{
    access_line_send(line, line->prompt);
    access_line_send(line, " ");
}

void access_line_send_prompt(access_line_t *line)
{
    send_prompt(line);
}

// ============================================================================
// Pager (--More--)
// ============================================================================

#define ACCESS_PAGER_PROMPT "--More--"

static uint32_t pager_count_lines(const char *text)
{
    uint32_t count = 0;
    for (const char *p = text; *p; p++)
    {
        if (*p == '\n')
        {
            count++;
        }
    }
    return count;
}

static void pager_show_more_prompt(access_line_t *line)
{
    access_line_send(line, ACCESS_PAGER_PROMPT);
}

static void pager_clear_more_prompt(access_line_t *line)
{
    access_line_send(line, "\r        \r");
}

static uint32_t pager_send_lines(access_line_t *line, uint32_t max_lines)
{
    if (!line->pager_buffer || line->pager_offset >= line->pager_total_len)
    {
        return 0;
    }

    uint32_t lines_sent = 0;
    uint32_t start = line->pager_offset;
    uint32_t pos = start;

    while (pos < line->pager_total_len && lines_sent < max_lines)
    {
        if (line->pager_buffer[pos] == '\n')
        {
            lines_sent++;
        }
        pos++;
    }

    if (pos > start)
    {
        access_line_send_data(line, line->pager_buffer + start, pos - start);
    }

    line->pager_offset = pos;
    return lines_sent;
}

static void access_pager_stop(access_line_t *line)
{
    if (!line)
    {
        return;
    }
    if (line->pager_active)
    {
        pager_clear_more_prompt(line);
    }
    if (line->pager_buffer)
    {
        g_free(line->pager_buffer);
        line->pager_buffer = NULL;
    }
    line->pager_offset = 0;
    line->pager_total_len = 0;
    line->pager_active = 0;
}

/** 分页输出整段文本：短则直接发，长则进入 --More-- 交互 */
void access_pager_output(access_line_t *line, const char *message)
{
    if (!line || !message || message[0] == '\0')
    {
        return;
    }

    if (line->pager_lines_per_page == 0)
    {
        access_line_send(line, message);
        return;
    }

    uint32_t lines = pager_count_lines(message);
    if (lines <= line->pager_lines_per_page)
    {
        access_line_send(line, message);
        return;
    }

    uint32_t msg_len = strlen(message);
    line->pager_buffer = g_malloc(msg_len + 1);
    memcpy(line->pager_buffer, message, msg_len + 1);
    line->pager_total_len = msg_len;
    line->pager_offset = 0;
    line->pager_active = 1;

    pager_send_lines(line, line->pager_lines_per_page);

    if (line->pager_offset < line->pager_total_len)
    {
        pager_show_more_prompt(line);
    }
    else
    {
        access_pager_stop(line);
    }
}

/** pager 激活时的按键处理；返回 1 表示按键被 pager 消费 */
static int pager_handle_key(access_line_t *line, char c)
{
    if (!line->pager_active)
    {
        return 0;
    }

    uint32_t page_size = line->pager_lines_per_page > 0 ? line->pager_lines_per_page : ACCESS_PAGER_DEFAULT_LINES;

    if (c == ' ')
    {
        pager_clear_more_prompt(line);
        pager_send_lines(line, page_size);
        if (line->pager_offset < line->pager_total_len)
        {
            pager_show_more_prompt(line);
        }
        else
        {
            access_pager_stop(line);
            send_prompt(line);
        }
    }
    else if (c == '\r' || c == '\n')
    {
        pager_clear_more_prompt(line);
        pager_send_lines(line, 1);
        if (line->pager_offset < line->pager_total_len)
        {
            pager_show_more_prompt(line);
        }
        else
        {
            access_pager_stop(line);
            send_prompt(line);
        }
    }
    else if (c == 'q' || c == 'Q')
    {
        access_pager_stop(line);
        send_prompt(line);
    }
    return 1;
}

// ============================================================================
// 行编辑 / 光标
// ============================================================================

#define ANSI_CLEAR_LINE "\x1B[2K"
#define ANSI_CURSOR_RIGHT "\x1B[C"

static void redraw_from_cursor(access_line_t *line, const char *buffer, uint32_t from_pos, uint32_t total_len)
{
    if (total_len > from_pos)
    {
        access_line_send_data(line, buffer + from_pos, total_len - from_pos);
    }
    access_line_send(line, " ");
    uint32_t move_back = total_len - from_pos + 1;
    for (uint32_t i = 0; i < move_back; i++)
    {
        access_line_send(line, "\b");
    }
}

static void clear_and_redraw_line(access_line_t *line, const char *buffer, uint32_t len, uint32_t cursor_pos)
{
    access_line_send(line, "\r");
    access_line_send(line, ANSI_CLEAR_LINE);
    send_prompt(line);
    if (len > 0)
    {
        access_line_send_data(line, buffer, len);
    }
    if (cursor_pos < len)
    {
        uint32_t move_back = len - cursor_pos;
        for (uint32_t i = 0; i < move_back; i++)
        {
            access_line_send(line, "\b");
        }
    }
}

static void handle_arrow_up(access_line_t *line, char *line_buffer, uint32_t *line_pos, uint32_t *cursor_pos)
{
    access_session_history_t *history = &line->history;
    if (history->count == 0)
    {
        return;
    }

    if (history->browse_idx == -1)
    {
        line_buffer[*line_pos] = '\0';
        g_strlcpy(history->temp_buffer, line_buffer, ACCESS_MAX_CMD_LEN);
        history->browse_idx = 0;
    }
    else if (history->browse_idx < (int32_t)(history->count - 1))
    {
        history->browse_idx++;
    }
    else
    {
        return;
    }

    const char *hist_cmd = access_session_history_get(history, history->browse_idx);
    if (hist_cmd)
    {
        g_strlcpy(line_buffer, hist_cmd, ACCESS_MAX_CMD_LEN);
        *line_pos = strlen(line_buffer);
        *cursor_pos = *line_pos;
        clear_and_redraw_line(line, line_buffer, *line_pos, *cursor_pos);
    }
}

static void handle_arrow_down(access_line_t *line, char *line_buffer, uint32_t *line_pos, uint32_t *cursor_pos)
{
    access_session_history_t *history = &line->history;
    if (history->browse_idx == -1)
    {
        return;
    }

    if (history->browse_idx > 0)
    {
        history->browse_idx--;
        const char *hist_cmd = access_session_history_get(history, history->browse_idx);
        if (hist_cmd)
        {
            g_strlcpy(line_buffer, hist_cmd, ACCESS_MAX_CMD_LEN);
            *line_pos = strlen(line_buffer);
            *cursor_pos = *line_pos;
            clear_and_redraw_line(line, line_buffer, *line_pos, *cursor_pos);
        }
    }
    else
    {
        history->browse_idx = -1;
        g_strlcpy(line_buffer, history->temp_buffer, ACCESS_MAX_CMD_LEN);
        *line_pos = strlen(line_buffer);
        *cursor_pos = *line_pos;
        clear_and_redraw_line(line, line_buffer, *line_pos, *cursor_pos);
    }
}

static void handle_arrow_left(access_line_t *line, uint32_t *cursor_pos)
{
    if (*cursor_pos > 0)
    {
        (*cursor_pos)--;
        access_line_send(line, "\b");
    }
}

static void handle_arrow_right(access_line_t *line, uint32_t line_pos, uint32_t *cursor_pos)
{
    if (*cursor_pos < line_pos)
    {
        access_line_send(line, ANSI_CURSOR_RIGHT);
        (*cursor_pos)++;
    }
}

// ============================================================================
// 命令提交 / 会话开关 —— 向 CLI 命令引擎发 RPC
// ============================================================================

/** 向 CLI 发 SESSION_OPEN，拿回 welcome + 初始提示符并显示 */
static void access_line_open_on_cli(access_line_t *line)
{
    access_session_open_t *req = g_new0(access_session_open_t, 1);
    req->line_id = line->line_id;
    req->line_type = line->line_type;
    req->client_port = line->client_port;
    g_strlcpy(req->client_ip, line->client_ip, sizeof(req->client_ip));

    dev_ipc_message_t *m = dev_ipc_message_create(ACCESS_MSG_SESSION_OPEN, DEV_MODULE_ID_ACCESS, DEV_MODULE_ID_CLI, 0,
                                                  req, sizeof(*req), g_free);
    if (!m)
    {
        g_free(req);
        send_prompt(line);
        return;
    }
    dev_ipc_message_t *resp = dev_ipc_query(access_ipc_ctx(), DEV_MODULE_ID_CLI, m, 5000);
    dev_ipc_message_free(m);

    if (resp && resp->payload && resp->payload_len >= sizeof(access_text_resp_t))
    {
        access_text_resp_t *r = (access_text_resp_t *)resp->payload;
        g_strlcpy(line->prompt, r->prompt, sizeof(line->prompt));
        if (r->text[0] != '\0')
        {
            access_line_send(line, r->text);
        }
    }
    if (resp)
    {
        dev_ipc_message_free(resp);
    }
    send_prompt(line);
}

/** 设置 vty 线区间 transport（per-line 接入门控）+ 回显。监听由全局 telnet server 开关控制，
 *  与此无关——这里只改"哪些线允许 telnet 接入"。 */
static void access_set_transport(access_line_t *line, uint32_t first, uint32_t last, uint8_t bits, const char *what)
{
    access_vty_set_transport(first, last, bits);

    /* 持久化区间内每条 vty 的 transport（best-effort，DB 未就绪则仅内存生效） */
    uint32_t f = first, l = last;
    if (f > l)
    {
        uint32_t t = f;
        f = l;
        l = t;
    }
    if (l >= ACCESS_VTY_COUNT)
    {
        l = ACCESS_VTY_COUNT - 1;
    }
    for (uint32_t i = f; i <= l && i < ACCESS_VTY_COUNT; i++)
    {
        access_db_save_vty_transport(i, bits);
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "transport input %s applied to vty %u-%u.\r\n", what, first, last);
    access_line_send(line, buf);
}

/** transport 位掩码 → 可读字符串 */
static const char *transport_str(uint8_t bits)
{
    if ((bits & ACCESS_TRANSPORT_TELNET) && (bits & ACCESS_TRANSPORT_SSH))
    {
        return "all";
    }
    if (bits & ACCESS_TRANSPORT_TELNET)
    {
        return "telnet";
    }
    if (bits & ACCESS_TRANSPORT_SSH)
    {
        return "ssh";
    }
    return "none";
}

/** 构建并分页输出 show line 表（线类型 / transport / 状态 / peer） */
static void access_show_line(access_line_t *line)
{
    GString *out = g_string_new("");
    g_string_append_printf(out, "\r\nTelnet server: %s\r\n\r\n",
                           access_telnet_server_enabled() ? "enabled (listening on port 23)" : "disabled");
    g_string_append(out, "  Line     Type  Transport  Status  Peer\r\n");
    g_string_append(out, "  -------  ----  ---------  ------  ---------------\r\n");

    g_mutex_lock(&g_pool_mutex);
    for (uint32_t i = 0; i < ACCESS_LINE_POOL_SIZE; i++)
    {
        access_line_t *l = &g_lines[i];
        char name[16];
        const char *trans;
        if (i == ACCESS_CON_LINE_ID)
        {
            g_strlcpy(name, "con 0", sizeof(name));
            trans = "-";
        }
        else
        {
            snprintf(name, sizeof(name), "vty %u", i - ACCESS_VTY_FIRST_ID);
            trans = transport_str(g_vty_transport[i - ACCESS_VTY_FIRST_ID]);
        }
        g_string_append_printf(out, "  %-7s  %-4s  %-9s  %-6s  %s\r\n", name, (i == ACCESS_CON_LINE_ID) ? "CON" : "VTY",
                               trans, l->in_use ? "active" : "idle", l->in_use ? l->client_ip : "-");
    }
    g_mutex_unlock(&g_pool_mutex);

    g_string_append(out, "\r\n");
    access_pager_output(line, out->str);
    g_string_free(out, TRUE);
}

/** 执行 ACCESS line 层本地命令（CLI 匹配后回传 group，由本线本地执行） */
static void access_exec_line_cmd(access_line_t *line, uint32_t line_cmd, uint32_t line_cmd_no, uint32_t arg1,
                                 uint32_t arg2)
{
    switch (line_cmd)
    {
        case ACCESS_LINE_CMD_BASH:
            /* 实际 PTY 桥接由 server 线程在 fd 上做（见 access_main.c） */
            line->enter_bash = 1;
            break;

        case ACCESS_LINE_CMD_TERMINAL_LENGTH:
            if (line_cmd_no)
            {
                line->pager_lines_per_page = ACCESS_PAGER_DEFAULT_LINES;
                access_line_send(line, "Pager restored to default for this line.\r\n");
            }
            else
            {
                line->pager_lines_per_page = 0;
                access_line_send(line, "Pager disabled for this line.\r\n");
            }
            break;

        case ACCESS_LINE_CMD_LINE_ENTER:
            /* 进入 line 视图：视图切换由 CLI 完成，ACCESS 无需动作 */
            break;

        case ACCESS_LINE_CMD_TRANSPORT_TELNET:
            access_set_transport(line, arg1, arg2, ACCESS_TRANSPORT_TELNET, "telnet");
            break;

        case ACCESS_LINE_CMD_TRANSPORT_SSH:
            access_set_transport(line, arg1, arg2, ACCESS_TRANSPORT_SSH, "ssh");
            access_line_send(line, "Note: ssh server not yet implemented; configuration recorded only.\r\n");
            break;

        case ACCESS_LINE_CMD_TRANSPORT_ALL:
            access_set_transport(line, arg1, arg2, ACCESS_TRANSPORT_TELNET | ACCESS_TRANSPORT_SSH, "all");
            break;

        case ACCESS_LINE_CMD_TRANSPORT_NONE:
            access_set_transport(line, arg1, arg2, 0, "none");
            break;

        case ACCESS_LINE_CMD_TELNET_SERVER:
            /* 全局监听开关：enable 起 23 监听，no/disable 关。per-line 仍各自控制能否接入。 */
            g_telnet_server_enabled = line_cmd_no ? 0 : 1;
            access_telnet_apply_gating();
            access_db_save_telnet_server(g_telnet_server_enabled);
            access_line_send(line,
                             g_telnet_server_enabled ? "Telnet server enabled.\r\n" : "Telnet server disabled.\r\n");
            break;

        case ACCESS_LINE_CMD_SHOW_LINE:
            access_show_line(line);
            break;

        default:
            break;
    }
}

static dev_ipc_message_t *access_line_query_input_continue(access_line_t *line)
{
    uint32_t *req = g_new(uint32_t, 1);
    *req = line->line_id;
    dev_ipc_message_t *m = dev_ipc_message_create(ACCESS_MSG_INPUT_CONTINUE, DEV_MODULE_ID_ACCESS, DEV_MODULE_ID_CLI, 0,
                                                  req, sizeof(*req), g_free);
    if (!m)
    {
        g_free(req);
        return NULL;
    }

    dev_ipc_message_t *resp = dev_ipc_query(access_ipc_ctx(), DEV_MODULE_ID_CLI, m, 65000);
    dev_ipc_message_free(m);
    return resp;
}

/** 一整行命令就绪：向 CLI 发 LINE_INPUT，拿回输出文本 + 新提示符 + close 标志 */
static void handle_line_submit(access_line_t *line)
{
    size_t cmd_len = strlen(line->line_buffer);
    size_t plen = sizeof(access_line_input_t) + cmd_len + 1;
    access_line_input_t *req = g_malloc0(plen);
    req->line_id = line->line_id;
    memcpy(req->cmdline, line->line_buffer, cmd_len + 1);

    dev_ipc_message_t *m =
        dev_ipc_message_create(ACCESS_MSG_LINE_INPUT, DEV_MODULE_ID_ACCESS, DEV_MODULE_ID_CLI, 0, req, plen, g_free);
    if (!m)
    {
        g_free(req);
        return;
    }
    /* 命令分发可能很慢（如 process reboot 等模块就绪），给 65s 超时。 */
    dev_ipc_message_t *resp = dev_ipc_query(access_ipc_ctx(), DEV_MODULE_ID_CLI, m, 65000);
    dev_ipc_message_free(m);

    if (!resp)
    {
        access_line_send(line, "% command engine timeout or unavailable\r\n");
        return;
    }

    GString *full_output = g_string_new("");
    uint32_t final_flags = 0;
    uint32_t final_line_cmd = ACCESS_LINE_CMD_NONE;
    uint32_t final_line_cmd_no = 0;
    uint32_t final_line_arg1 = 0;
    uint32_t final_line_arg2 = 0;
    char final_prompt[ACCESS_PROMPT_MAX_LEN] = "";
    int final_resp_seen = 0;

    while (resp)
    {
        if (!resp->payload || resp->payload_len < sizeof(access_text_resp_t))
        {
            g_string_append(full_output, "% invalid command engine response\r\n");
            dev_ipc_message_free(resp);
            break;
        }

        access_text_resp_t *r = (access_text_resp_t *)resp->payload;
        size_t text_cap = resp->payload_len - sizeof(access_text_resp_t);
        if (!memchr(r->text, '\0', text_cap))
        {
            g_string_append(full_output, "% invalid command engine response\r\n");
            dev_ipc_message_free(resp);
            break;
        }

        if (r->text[0] != '\0')
        {
            g_string_append(full_output, r->text);
        }

        int has_more = (r->flags & ACCESS_RESP_FLAG_MORE) ? 1 : 0;
        if (!has_more)
        {
            final_flags = r->flags;
            final_line_cmd = r->line_cmd;
            final_line_cmd_no = r->line_cmd_no;
            final_line_arg1 = r->line_arg1;
            final_line_arg2 = r->line_arg2;
            g_strlcpy(final_prompt, r->prompt, sizeof(final_prompt));
            final_resp_seen = 1;
        }

        dev_ipc_message_free(resp);
        resp = NULL;

        if (!has_more)
        {
            break;
        }

        resp = access_line_query_input_continue(line);
        if (!resp)
        {
            g_string_append(full_output, "% command engine timeout or unavailable\r\n");
            break;
        }
    }

    if (final_resp_seen)
    {
        g_strlcpy(line->prompt, final_prompt, sizeof(line->prompt));
    }
    if (full_output->len > 0)
    {
        access_pager_output(line, full_output->str);
    }
    if (final_resp_seen && (final_flags & ACCESS_RESP_FLAG_CLOSE_SESSION))
    {
        line->close_requested = 1;
    }
    if (final_resp_seen && final_line_cmd != ACCESS_LINE_CMD_NONE)
    {
        access_exec_line_cmd(line, final_line_cmd, final_line_cmd_no, final_line_arg1, final_line_arg2);
    }

    g_string_free(full_output, TRUE);
}

void access_line_close_on_cli(uint32_t line_id)
{
    uint32_t *req = g_new(uint32_t, 1);
    *req = line_id;
    dev_ipc_message_t *m = dev_ipc_message_create(ACCESS_MSG_SESSION_CLOSE, DEV_MODULE_ID_ACCESS, DEV_MODULE_ID_CLI, 0,
                                                  req, sizeof(uint32_t), g_free);
    if (!m)
    {
        g_free(req);
        return;
    }
    dev_ipc_message_t *resp = dev_ipc_query(access_ipc_ctx(), DEV_MODULE_ID_CLI, m, 3000);
    dev_ipc_message_free(m);
    if (resp)
    {
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// Tab 补全 / '?' 帮助 —— 匹配在 CLI，循环/渲染在本地
// ============================================================================

/** 向 CLI 发 TAB_REQ，解析候选 token 列表（GPtrArray，元素 g_strdup） */
static GPtrArray *access_tab_query(access_line_t *line, const char *partial)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    size_t plen = sizeof(access_line_input_t) + strlen(partial) + 1;
    access_line_input_t *req = g_malloc0(plen);
    req->line_id = line->line_id;
    memcpy(req->cmdline, partial, strlen(partial) + 1);

    dev_ipc_message_t *m =
        dev_ipc_message_create(ACCESS_MSG_TAB_REQ, DEV_MODULE_ID_ACCESS, DEV_MODULE_ID_CLI, 0, req, plen, g_free);
    if (!m)
    {
        g_free(req);
        return arr;
    }
    dev_ipc_message_t *resp = dev_ipc_query(access_ipc_ctx(), DEV_MODULE_ID_CLI, m, 5000);
    dev_ipc_message_free(m);

    if (resp && resp->payload && resp->payload_len > sizeof(access_tab_resp_t))
    {
        access_tab_resp_t *r = (access_tab_resp_t *)resp->payload;
        if (r->kind == ACCESS_TAB_KIND_TOKENS)
        {
            size_t datalen = resp->payload_len - sizeof(access_tab_resp_t);
            const char *p = r->data;
            const char *end = r->data + datalen;
            while (p < end && *p != '\0')
            {
                g_ptr_array_add(arr, g_strdup(p));
                p += strlen(p) + 1;
            }
        }
    }
    if (resp)
    {
        dev_ipc_message_free(resp);
    }
    return arr;
}

/** 向 CLI 发 HELP_REQ，返回帮助文本（g_strdup，调用方 g_free） */
static char *access_help_query(access_line_t *line, const char *partial)
{
    size_t plen = sizeof(access_line_input_t) + strlen(partial) + 1;
    access_line_input_t *req = g_malloc0(plen);
    req->line_id = line->line_id;
    memcpy(req->cmdline, partial, strlen(partial) + 1);

    dev_ipc_message_t *m =
        dev_ipc_message_create(ACCESS_MSG_HELP_REQ, DEV_MODULE_ID_ACCESS, DEV_MODULE_ID_CLI, 0, req, plen, g_free);
    if (!m)
    {
        g_free(req);
        return NULL;
    }
    dev_ipc_message_t *resp = dev_ipc_query(access_ipc_ctx(), DEV_MODULE_ID_CLI, m, 5000);
    dev_ipc_message_free(m);

    char *text = NULL;
    if (resp && resp->payload && resp->payload_len > 0)
    {
        text = g_strdup((const char *)resp->payload);
    }
    if (resp)
    {
        dev_ipc_message_free(resp);
    }
    return text;
}

/** 把候选 token 应用到当前行：替换光标前最后一个 token，可选追加空格 */
static void access_tab_apply(access_line_t *line, const char *candidate, int append_space)
{
    uint32_t st = line->cursor_pos;
    while (st > 0 && line->line_buffer[st - 1] != ' ')
    {
        st--;
    }
    char newbuf[ACCESS_MAX_CMD_LEN];
    uint32_t n = st;
    memcpy(newbuf, line->line_buffer, st);
    uint32_t reserve = append_space ? 2u : 1u;
    for (const char *c = candidate; *c && n < ACCESS_MAX_CMD_LEN - reserve; c++)
    {
        newbuf[n++] = *c;
    }
    if (append_space && n < ACCESS_MAX_CMD_LEN - 1)
    {
        newbuf[n++] = ' ';
    }
    newbuf[n] = '\0';
    memcpy(line->line_buffer, newbuf, n + 1);
    line->line_pos = n;
    line->cursor_pos = n;
}

static void handle_tab(access_line_t *line)
{
    /* 循环态下用原始输入做匹配，保证候选集稳定 */
    char partial[ACCESS_MAX_CMD_LEN];
    uint32_t src_len = line->tab_cycling ? line->tab_original_pos : line->cursor_pos;
    const char *src = line->tab_cycling ? line->tab_original : line->line_buffer;
    if (src_len >= ACCESS_MAX_CMD_LEN)
    {
        src_len = ACCESS_MAX_CMD_LEN - 1;
    }
    memcpy(partial, src, src_len);
    partial[src_len] = '\0';

    GPtrArray *cands = access_tab_query(line, partial);
    if (cands->len == 0)
    {
        line->tab_cycling = 0;
        access_line_send(line, "\r\n");
        send_prompt(line);
        access_line_send_data(line, line->line_buffer, line->line_pos);
        g_ptr_array_free(cands, TRUE);
        return;
    }

    if (cands->len == 1)
    {
        line->tab_cycling = 0;
        access_line_send(line, "\r\n");
        send_prompt(line);
        access_tab_apply(line, (const char *)g_ptr_array_index(cands, 0), 1);
        access_line_send(line, line->line_buffer);
    }
    else
    {
        if (!line->tab_cycling)
        {
            line->tab_cycling = 1;
            line->tab_match_index = 0;
            memcpy(line->tab_original, line->line_buffer, line->cursor_pos);
            line->tab_original_pos = line->cursor_pos;
        }
        else
        {
            line->tab_match_index = (line->tab_match_index + 1) % cands->len;
        }
        /* 还原到原始输入再套用当前候选 */
        memcpy(line->line_buffer, line->tab_original, line->tab_original_pos);
        line->line_pos = line->tab_original_pos;
        line->cursor_pos = line->tab_original_pos;
        line->line_buffer[line->line_pos] = '\0';

        access_line_send(line, "\r\n");
        send_prompt(line);
        access_tab_apply(line, (const char *)g_ptr_array_index(cands, line->tab_match_index), 0);
        access_line_send(line, line->line_buffer);
    }
    g_ptr_array_free(cands, TRUE);
}

static void handle_help(access_line_t *line)
{
    char partial[ACCESS_MAX_CMD_LEN];
    uint32_t n = line->cursor_pos;
    if (n >= ACCESS_MAX_CMD_LEN)
    {
        n = ACCESS_MAX_CMD_LEN - 1;
    }
    memcpy(partial, line->line_buffer, n);
    partial[n] = '\0';

    char *text = access_help_query(line, partial);
    access_line_send(line, "\r\n");
    if (text && text[0] != '\0')
    {
        access_pager_output(line, text);
    }
    g_free(text);

    line->line_pos = line->cursor_pos;
    line->line_buffer[line->line_pos] = '\0';

    /* pager 激活时由 pager 收尾重画提示符 */
    if (!line->pager_active)
    {
        clear_and_redraw_line(line, line->line_buffer, line->line_pos, line->cursor_pos);
    }
}

// ============================================================================
// 输入处理
// ============================================================================

void access_line_greet(access_line_t *line)
{
    /* telnet 字符模式协商仅对 telnet/vty 线发送；console（unix socket + netnexus-console
     * raw 客户端）不懂 IAC，发了会显示成乱码，故跳过。 */
    if (line->line_type == ACCESS_LINE_TYPE_VTY)
    {
        unsigned char telnet_opts[] = {
            255, 251, 1,  // IAC WILL ECHO
            255, 251, 3,  // IAC WILL SUPPRESS_GO_AHEAD
            255, 253, 34, // IAC DO LINEMODE
        };
        access_line_send_data(line, telnet_opts, sizeof(telnet_opts));
    }

    /* welcome + 初始提示符由 CLI 命令引擎下发（含 sysname 渲染）。 */
    access_line_open_on_cli(line);
}

int access_line_process_input(access_line_t *line)
{
    char c;
    ssize_t n;

    while ((n = read(line->fd, &c, 1)) > 0)
    {
        /* 过滤 Telnet IAC 协商字节（telnet 传输专属）。 */
        if ((unsigned char)c == 255)
        {
            char cmd;
            if (read(line->fd, &cmd, 1) < 1)
            {
                continue;
            }
            unsigned char ucmd = (unsigned char)cmd;
            if (ucmd == 250) // SB：读到 IAC SE
            {
                char prev = 0, cur;
                while (read(line->fd, &cur, 1) == 1)
                {
                    if ((unsigned char)prev == 255 && (unsigned char)cur == 240)
                    {
                        break;
                    }
                    prev = cur;
                }
            }
            else if (ucmd >= 251 && ucmd <= 254) // WILL/WONT/DO/DONT + option
            {
                char opt;
                if (read(line->fd, &opt, 1) < 1)
                {
                    continue;
                }
            }
            continue;
        }

        if (line->pager_active)
        {
            pager_handle_key(line, c);
            continue;
        }

        if (line->state == ACCESS_INPUT_NORMAL)
        {
            if (c == 27)
            {
                line->state = ACCESS_INPUT_ESC;
                continue;
            }

            if (c != '\t')
            {
                line->tab_cycling = 0;
            }

            if (c == '\r' || c == '\n')
            {
                access_line_send(line, "\r\n");
                if (line->line_pos > 0)
                {
                    line->line_buffer[line->line_pos] = '\0';
                    access_session_history_add(&line->history, line->line_buffer, line->client_ip);
                    g_mutex_lock(&g_pool_mutex);
                    access_global_history_add(&g_global_history, line->line_buffer, line->client_ip);
                    g_mutex_unlock(&g_pool_mutex);

                    line->line_pos = 0;
                    line->cursor_pos = 0;
                    line->history.browse_idx = -1;

                    handle_line_submit(line);

                    /* bash line 命令：server 线程接管本线 fd 做 PTY 桥接，不发提示符 */
                    if (line->enter_bash)
                    {
                        return 0;
                    }
                }
                if (line->close_requested)
                {
                    return -1;
                }
                if (!line->pager_active)
                {
                    send_prompt(line);
                }
            }
            else if (c == 127 || c == 8) // Backspace
            {
                if (line->cursor_pos > 0)
                {
                    if (line->cursor_pos < line->line_pos)
                    {
                        memmove(line->line_buffer + line->cursor_pos - 1, line->line_buffer + line->cursor_pos,
                                line->line_pos - line->cursor_pos);
                        line->line_pos--;
                        line->cursor_pos--;
                        access_line_send(line, "\b");
                        redraw_from_cursor(line, line->line_buffer, line->cursor_pos, line->line_pos);
                    }
                    else
                    {
                        line->line_pos--;
                        line->cursor_pos--;
                        access_line_send(line, "\b \b");
                    }
                }
            }
            else if (c == '\t')
            {
                handle_tab(line);
            }
            else if (c == '?')
            {
                handle_help(line);
            }
            else if (line->line_pos < ACCESS_MAX_CMD_LEN - 1 && c >= 32 && c < 127)
            {
                if (line->cursor_pos < line->line_pos)
                {
                    memmove(line->line_buffer + line->cursor_pos + 1, line->line_buffer + line->cursor_pos,
                            line->line_pos - line->cursor_pos);
                    line->line_buffer[line->cursor_pos] = c;
                    line->line_pos++;
                    line->cursor_pos++;
                    access_line_send_data(line, &c, 1);
                    redraw_from_cursor(line, line->line_buffer, line->cursor_pos, line->line_pos);
                }
                else
                {
                    line->line_buffer[line->line_pos++] = c;
                    line->cursor_pos++;
                    access_line_send_data(line, &c, 1);
                }
            }
        }
        else if (line->state == ACCESS_INPUT_ESC)
        {
            line->state = (c == '[') ? ACCESS_INPUT_CSI : ACCESS_INPUT_NORMAL;
        }
        else if (line->state == ACCESS_INPUT_CSI)
        {
            if (c == 'A')
            {
                handle_arrow_up(line, line->line_buffer, &line->line_pos, &line->cursor_pos);
            }
            else if (c == 'B')
            {
                handle_arrow_down(line, line->line_buffer, &line->line_pos, &line->cursor_pos);
            }
            else if (c == 'C')
            {
                handle_arrow_right(line, line->line_pos, &line->cursor_pos);
            }
            else if (c == 'D')
            {
                handle_arrow_left(line, &line->cursor_pos);
            }
            line->state = ACCESS_INPUT_NORMAL;
        }
    }

    if (n == 0)
    {
        return -1; // 断开
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        return -1; // 出错
    }
    return 0;
}
