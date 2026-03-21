/**
 * @file   cli_cfg.c
 * @brief  CFG 模块 CLI 命令处理，处理 show、exit、config 等核心命令
 * @author jhb
 * @date   2026/01/22
 */

#include "cli_cfg.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli_handler.h"
#include "cli_main.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 命令树打印
// ============================================================================

static void print_commands_recursive(GString *out, const char *view_name, const char *prefix, cli_tree_node_t *node)
{
    if (!node)
    {
        return;
    }

    char new_prefix[MAX_CMD_LEN];
    if (strlen(prefix) > 0)
    {
        snprintf(new_prefix, sizeof(new_prefix), "%s %s", prefix, node->name ? node->name : "");
    }
    else
    {
        strncpy(new_prefix, node->name ? node->name : "", sizeof(new_prefix) - 1);
        new_prefix[sizeof(new_prefix) - 1] = '\0';
    }

    if (node->is_end_node == TRUE)
    {
        char module_name[DEV_MODULE_NAME_MAX_LEN];
        if (dev_get_module_name(g_cli_local->dev_ipc_ctx, node->module_id, module_name) != ERRCODE_SUCCESS)
        {
            snprintf(module_name, sizeof(module_name), "unknown");
        }
        char buffer[2048];
        snprintf(buffer, sizeof(buffer), "  %-15s %-15s %s\r\n", view_name, module_name, new_prefix);
        g_string_append(out, buffer);
    }

    for (uint32_t i = 0; i < node->num_children; i++)
    {
        print_commands_recursive(out, view_name, new_prefix, node->children[i]);
    }
}

static void print_view_commands_flat(cli_view_node_t *view, GString *out)
{
    if (!view)
    {
        return;
    }

    if (view->cmd_tree)
    {
        for (uint32_t i = 0; i < view->cmd_tree->num_children; i++)
        {
            print_commands_recursive(out, view->view_name, "", view->cmd_tree->children[i]);
        }
    }

    for (uint32_t i = 0; i < view->num_children; i++)
    {
        print_view_commands_flat(view->children[i], out);
    }
}

// ============================================================================
// 分批输出辅助
// ============================================================================

static void cli_chunk_output(GString *full, cli_resp_out_t *resp_out)
{
    uint32_t offset = resp_out->batch_offset;
    if (offset >= full->len)
    {
        resp_out->message[0] = '\0';
        resp_out->has_more = 0;
        return;
    }

    size_t remaining = full->len - offset;
    size_t chunk = remaining < (CLI_MAX_RESP_LEN - 1) ? remaining : (CLI_MAX_RESP_LEN - 1);
    memcpy(resp_out->message, full->str + offset, chunk);
    resp_out->message[chunk] = '\0';
    resp_out->batch_offset = offset + chunk;

    resp_out->has_more = (resp_out->batch_offset < full->len) ? 1 : 0;
}

/* 缓存 show 输出 */
static GString *g_cli_show_cache = NULL;

/* 缓存 history 输出 */
static GString *g_cli_history_cache = NULL;

// ============================================================================
// 按 group_id 分发的 handler
// ============================================================================

/**
 * @brief show cli command-info (group_id=1)
 */
static void handle_show_commands(cli_session_t *session)
{
    GString *full_output = g_string_new("");

    cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));

    /* 生成完整输出到缓存 */
    if (g_cli_show_cache)
    {
        g_string_free(g_cli_show_cache, TRUE);
    }
    g_cli_show_cache = g_string_new("");

    g_string_append(g_cli_show_cache, "\r\nCLI Commands List:\r\n");
    g_string_append(g_cli_show_cache, "===================\r\n");
    g_string_append(g_cli_show_cache, "  VIEW            MODULE          COMMAND\r\n");
    g_string_append(g_cli_show_cache, "  ----            ------          -------\r\n");

    if (g_cli_local->view_tree.root)
    {
        print_view_commands_flat(g_cli_local->view_tree.root, g_cli_show_cache);
    }

    /* 全局命令不再克隆进各视图，单独打印 global_view */
    if (g_cli_local->view_tree.global_view)
    {
        print_view_commands_flat(g_cli_local->view_tree.global_view, g_cli_show_cache);
    }

    g_string_append(g_cli_show_cache, "\r\n");

    /* 分批读取 */
    do
    {
        resp_out.message[0] = '\0';
        resp_out.has_more = 0;
        cli_chunk_output(g_cli_show_cache, &resp_out);
        if (resp_out.message[0] != '\0')
        {
            g_string_append(full_output, resp_out.message);
        }
    } while (resp_out.has_more);

    g_string_free(g_cli_show_cache, TRUE);
    g_cli_show_cache = NULL;

    if (full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }
    g_string_free(full_output, TRUE);
}

/**
 * @brief show cli history (group_id=2)
 */
static void handle_show_history(cli_session_t *session)
{
    GString *full_output = g_string_new("");
    cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));

    if (g_cli_history_cache)
    {
        g_string_free(g_cli_history_cache, TRUE);
    }
    g_cli_history_cache = g_string_new("");
    char buffer[512];

    pthread_mutex_lock(&g_cli_local->history_mutex);

    g_string_append(g_cli_history_cache, "\r\n");
    g_string_append(g_cli_history_cache, "Command History:\r\n");
    g_string_append(g_cli_history_cache,
                    "================================================================================\r\n");
    g_string_append(g_cli_history_cache, " No  Time                Command                          Client IP\r\n");
    g_string_append(g_cli_history_cache,
                    "--------------------------------------------------------------------------------\r\n");

    for (uint32_t i = 0; i < g_cli_local->global_history.count; i++)
    {
        const cli_history_entry_t *entry =
            cli_global_history_get_entry(&g_cli_local->global_history, g_cli_local->global_history.count - 1 - i);
        if (entry && entry->command)
        {
            struct tm *timeinfo = localtime(&entry->timestamp);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);

            char cmd_display[33];
            if (strlen(entry->command) > 32)
            {
                strncpy(cmd_display, entry->command, 29);
                cmd_display[29] = '.';
                cmd_display[30] = '.';
                cmd_display[31] = '.';
                cmd_display[32] = '\0';
            }
            else
            {
                strcpy(cmd_display, entry->command);
            }

            snprintf(buffer, sizeof(buffer), " %-3u %-19s %-32s %-15s\r\n", i + 1, time_str, cmd_display,
                     entry->client_ip);
            g_string_append(g_cli_history_cache, buffer);
        }
    }

    g_string_append(g_cli_history_cache,
                    "================================================================================\r\n");
    snprintf(buffer, sizeof(buffer), "Total: %u command(s)\r\n\r\n", g_cli_local->global_history.count);
    g_string_append(g_cli_history_cache, buffer);

    pthread_mutex_unlock(&g_cli_local->history_mutex);

    do
    {
        resp_out.message[0] = '\0';
        resp_out.has_more = 0;
        cli_chunk_output(g_cli_history_cache, &resp_out);
        if (resp_out.message[0] != '\0')
        {
            g_string_append(full_output, resp_out.message);
        }
    } while (resp_out.has_more);

    g_string_free(g_cli_history_cache, TRUE);
    g_cli_history_cache = NULL;

    if (full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }
    g_string_free(full_output, TRUE);
}

/**
 * @brief 拉取单个模块 show current-configuration 的完整输出（支持 RESP_MORE/CONTINUE）
 */
static void collect_module_show_config(uint32_t mod_id, GString *output)
{
    dev_ipc_message_t *req =
        dev_ipc_message_create(CLI_MSG_TYPE_SHOW_CONFIG, DEV_MODULE_ID_CLI, mod_id, 0, NULL, 0, NULL);
    if (!req)
    {
        return;
    }

    /* 保护上限：防止异常模块无限 RESP_MORE */
    const uint32_t max_chunks = 4096;
    uint32_t chunks = 0;

    while (req && chunks < max_chunks)
    {
        /* 首包用较短超时，避免不支持 SHOW_CONFIG 的模块拖慢整体 */
        uint32_t timeout_ms = (chunks == 0) ? 1000 : 5000;
        dev_ipc_message_t *resp = dev_ipc_query(g_cli_local->dev_ipc_ctx, mod_id, req, timeout_ms);
        dev_ipc_message_free(req);
        req = NULL;
        chunks++;

        if (!resp)
        {
            LOG_WARN("show current-configuration: module 0x%08X query timeout", mod_id);
            break;
        }

        if (resp->msg_type == CLI_MSG_TYPE_RESP || resp->msg_type == CLI_MSG_TYPE_RESP_MORE)
        {
            /* payload 为 NULL 结尾字符串，payload_len > 1 才有实际内容 */
            if (resp->payload && resp->payload_len > 1)
            {
                g_string_append(output, (const char *)resp->payload);
            }

            if (resp->msg_type == CLI_MSG_TYPE_RESP_MORE)
            {
                req = dev_ipc_message_create(CLI_MSG_TYPE_CONTINUE, DEV_MODULE_ID_CLI, mod_id, 0, NULL, 0, NULL);
                if (!req)
                {
                    LOG_WARN("show current-configuration: create CONTINUE failed for module 0x%08X", mod_id);
                }
            }
        }
        else
        {
            LOG_WARN("show current-configuration: module 0x%08X returned unexpected msg_type=0x%08X", mod_id,
                     resp->msg_type);
        }

        dev_ipc_message_free(resp);
    }

    if (req)
    {
        dev_ipc_message_free(req);
    }

    if (chunks >= max_chunks)
    {
        LOG_WARN("show current-configuration: module 0x%08X exceeded max chunks(%u), stop collecting", mod_id,
                 max_chunks);
    }
}

static gint cmp_uint32_asc(gconstpointer a, gconstpointer b)
{
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    if (va < vb)
    {
        return -1;
    }
    if (va > vb)
    {
        return 1;
    }
    return 0;
}

/**
 * @brief 动态收集当前与 CLI 已建立 IPC 的模块 ID（去重、升序）
 */
static GArray *collect_connected_modules_for_show_config(void)
{
    GArray *modules = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    if (!g_cli_local || !g_cli_local->dev_ipc_ctx)
    {
        return modules;
    }

    dev_ipc_context_t *ctx = g_cli_local->dev_ipc_ctx;
    pthread_mutex_lock(&ctx->comutex);
    for (int i = 0; i < ctx->num_connections; i++)
    {
        dev_ipc_connection_t *conn = ctx->connections[i];
        if (!conn || conn->state != DEV_IPC_COCONNECTED)
        {
            continue;
        }

        uint32_t mod_id = conn->remote_module_id;
        if (mod_id == DEV_MODULE_ID_CLI)
        {
            continue;
        }

        gboolean exists = FALSE;
        for (guint j = 0; j < modules->len; j++)
        {
            if (g_array_index(modules, uint32_t, j) == mod_id)
            {
                exists = TRUE;
                break;
            }
        }

        if (!exists)
        {
            g_array_append_val(modules, mod_id);
        }
    }
    pthread_mutex_unlock(&ctx->comutex);

    g_array_sort(modules, cmp_uint32_asc);
    return modules;
}

/**
 * @brief show current-configuration (group_id=3)
 *
 * 向所有业务模块发送 CLI_MSG_TYPE_SHOW_CONFIG，收集响应并聚合输出。
 * not connected的模块直接跳过，避免超时等待。
 */
static void handle_show_config(cli_session_t *session)
{
    GString *output = g_string_new("");
    GArray *modules = collect_connected_modules_for_show_config();
    for (guint i = 0; i < modules->len; i++)
    {
        uint32_t mod_id = g_array_index(modules, uint32_t, i);
        /* 先完整拉取当前模块，再切下一个模块 */
        collect_module_show_config(mod_id, output);
    }
    g_array_free(modules, TRUE);

    if (output->len > 0)
    {
        cli_pager_output(session, output->str);
    }
    else
    {
        cli_send_message(session, "No configuration found.\r\n");
    }

    g_string_free(output, TRUE);
}

/**
 * @brief exit (group_id=4)
 */
static void handle_op_exit(cli_session_t *session)
{
    cli_view_node_t *parent_view = session->current_view->parent;
    if (parent_view == NULL)
    {
        close(session->client_fd);
    }
    else
    {
        /* parent_view 指针即目标视图，直接使用 */
        session->current_view = parent_view;
        cli_prompt_pop(session);
    }
}

/**
 * @brief config (group_id=5)
 */
static void handle_op_config(cli_session_t *session)
{
    cli_view_node_t *config_view = cli_view_find_by_name(g_cli_local->view_tree.root, CLI_VIEW_CONFIG);
    if (config_view)
    {
        cli_prompt_push(session);
        session->current_view = config_view;
        update_prompt_from_template(session, session->current_view->prompt_template);
    }
}

// ============================================================================
// Bash 模式：转发 telnet 会话与 PTY 之间的数据
// ============================================================================

/**
 * @brief bash 桥接线程上下文
 */
typedef struct
{
    cli_session_t *session; /**< 当前 CLI 会话 */
    int client_fd;          /**< 客户端 socket fd */
} bash_bridge_ctx_t;

/**
 * @brief bash 桥接线程：在客户端 socket 与 bash PTY 之间双向转发数据
 */
static void *bash_bridge_thread(void *arg)
{
    pthread_setname_np(pthread_self(), "cfg-bash");
    bash_bridge_ctx_t *ctx = (bash_bridge_ctx_t *)arg;
    cli_session_t *session = ctx->session;
    int client_fd = ctx->client_fd;
    g_free(ctx);

    /* 创建 PTY 并 fork bash */
    int pty_master;
    struct winsize ws = {.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
    pid_t pid = forkpty(&pty_master, NULL, NULL, &ws);

    if (pid < 0)
    {
        /* fork 失败，恢复 epoll 监听 */
        cli_send_message(session, "Error: Failed to start bash.\r\n");
        session->bash_mode = 0;
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = client_fd;
        epoll_ctl(g_cli_local->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
        send_prompt(session);
        return NULL;
    }

    if (pid == 0)
    {
        /* 子进程：执行 bash */
        setenv("TERM", "xterm", 1);
        execlp("/bin/bash", "bash", "--login", NULL);
        _exit(1);
    }

    /* 父进程：双向数据转发 */
    char buf[4096];
    struct pollfd fds[2];

    while (1)
    {
        fds[0].fd = client_fd;
        fds[0].events = POLLIN;
        fds[1].fd = pty_master;
        fds[1].events = POLLIN;

        int ret = poll(fds, 2, 500);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        /* 检查 bash 是否已退出 */
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid)
        {
            pid = -1;
            break;
        }

        if (fds[0].revents & POLLIN)
        {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0)
            {
                /* 客户端断开 */
                break;
            }
            if (write(pty_master, buf, (size_t)n) < 0)
            {
                break;
            }
        }

        if (fds[1].revents & POLLIN)
        {
            ssize_t n = read(pty_master, buf, sizeof(buf));
            if (n <= 0)
            {
                /* bash 已退出或 PTY 关闭 */
                break;
            }
            if (write(client_fd, buf, (size_t)n) < 0)
            {
                break;
            }
        }
    }

    /* 清理：等待 bash 子进程退出 */
    if (pid > 0)
    {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
    close(pty_master);

    /* 恢复 epoll 监听，返回 CLI */
    session->bash_mode = 0;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = client_fd;
    epoll_ctl(g_cli_local->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

    cli_send_message(session, "\r\nBash session ended, returning to CLI...\r\n");
    send_prompt(session);

    return NULL;
}

/**
 * @brief bash (group_id=7)：暂时将客户端 socket 移出 epoll，由后台线程桥接 bash PTY
 */
static void handle_op_bash(cli_session_t *session)
{
    /* 从 epoll 中移除，防止主线程继续读取该 socket */
    epoll_ctl(g_cli_local->epoll_fd, EPOLL_CTL_DEL, session->client_fd, NULL);

    /* 标记会话进入 bash 模式（主循环不再发送 CLI 提示符） */
    session->bash_mode = 1;

    cli_send_message(session, "\r\nEntering bash shell, type 'exit' to return to CLI.\r\n\r\n");

    /* 创建桥接线程 */
    bash_bridge_ctx_t *ctx = g_malloc(sizeof(*ctx));
    ctx->session = session;
    ctx->client_fd = session->client_fd;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&tid, &attr, bash_bridge_thread, ctx) != 0)
    {
        /* 线程创建失败，立即恢复 */
        g_free(ctx);
        session->bash_mode = 0;
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = session->client_fd;
        epoll_ctl(g_cli_local->epoll_fd, EPOLL_CTL_ADD, session->client_fd, &ev);
        cli_send_message(session, "Error: Failed to create bash thread.\r\n");
    }

    pthread_attr_destroy(&attr);
}

/**
 * @brief show cli context (group_id=8)：打印当前会话上下文 TLV 中所有变量
 */
static void handle_show_context(cli_session_t *session)
{
    uint32_t ctx_len = 0;
    const uint8_t *ctx = cli_context_get(session, &ctx_len);

    GString *out = g_string_new("\r\nCLI Context Variables:\r\n");
    g_string_append(out, "  ctx-id  type  value\r\n");
    g_string_append(out, "  ------  ----  -----\r\n");

    if (!ctx || ctx_len < 2)
    {
        g_string_append(out, "  (no context)\r\n");
        cli_pager_output(session, out->str);
        g_string_free(out, TRUE);
        return;
    }

    uint16_t num_be;
    memcpy(&num_be, ctx, 2);
    uint16_t num = ntohs(num_be);
    uint32_t pos = 2;

    if (num == 0)
    {
        g_string_append(out, "  (no context)\r\n");
    }

    for (uint16_t i = 0; i < num && pos < ctx_len; i++)
    {
        if (pos + 4 > ctx_len)
        {
            break;
        }
        uint32_t id_be;
        memcpy(&id_be, ctx + pos, 4);
        uint32_t id = ntohl(id_be);
        pos += 4;

        if (pos >= ctx_len)
        {
            break;
        }
        uint8_t type = ctx[pos++];

        if (pos + 2 > ctx_len)
        {
            break;
        }
        uint16_t len_be;
        memcpy(&len_be, ctx + pos, 2);
        uint16_t elen = ntohs(len_be);
        pos += 2;

        char val_buf[128] = "(invalid)";
        if (type == CLI_TLV_TYPE_CTX && elen == 4 && pos + 4 <= ctx_len)
        {
            uint32_t v;
            memcpy(&v, ctx + pos, 4);
            snprintf(val_buf, sizeof(val_buf), "%u", ntohl(v));
        }
        else if (type == CLI_TLV_TYPE_CTX_STR && pos + elen <= ctx_len)
        {
            uint16_t copy_len = elen < (uint16_t)(sizeof(val_buf) - 3) ? elen : (uint16_t)(sizeof(val_buf) - 3);
            val_buf[0] = '"';
            memcpy(val_buf + 1, ctx + pos, copy_len);
            val_buf[1 + copy_len] = '"';
            val_buf[2 + copy_len] = '\0';
        }

        const char *type_str = (type == CLI_TLV_TYPE_CTX) ? "INT" : (type == CLI_TLV_TYPE_CTX_STR) ? "STR" : "???";
        char line[256];
        snprintf(line, sizeof(line), "  %-6u  %-4s  %s\r\n", id, type_str, val_buf);
        g_string_append(out, line);

        pos += elen;
    }

    g_string_append(out, "\r\n");
    cli_pager_output(session, out->str);
    g_string_free(out, TRUE);
}

/**
 * @brief terminal length 0 (group_id=9)：关闭当前会话分页输出
 */
static void handle_terminal_length_zero(cli_session_t *session)
{
    if (!session)
    {
        return;
    }

    session->pager_lines_per_page = 0;
    cli_pager_stop(session);
    cli_send_message(session, "CLI pager disabled for this session.\r\n");
}

/**
 * @brief end (group_id=6)
 */
static void handle_op_end(cli_session_t *session)
{
    cli_view_node_t *user_view = cli_view_find_by_name(g_cli_local->view_tree.root, CLI_VIEW_USER);
    if (user_view)
    {
        session->current_view = user_view;
        /* 释放所有上下文栈数据并重置 prompt 栈 */
        for (uint32_t i = 0; i < session->prompt_stack_depth; i++)
        {
            if (session->view_context_stack[i])
            {
                g_free(session->view_context_stack[i]);
                session->view_context_stack[i] = NULL;
                session->view_context_len[i] = 0;
            }
        }
        session->prompt_stack_depth = 0;
        update_prompt_from_template(session, session->current_view->prompt_template);
    }
}

// ============================================================================
// 主入口
// ============================================================================

int cli_handle(dev_ipc_message_t *msg, cli_session_t *session)
{
    if (!msg || !msg->payload || !session)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("Payload parsing failed");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("Received command (group_id=%u)", parser.group_id);

    switch (parser.group_id)
    {
        case CLI_GROUP_ID_SHOW_COMMANDS:
            handle_show_commands(session);
            break;
        case CLI_GROUP_ID_SHOW_HISTORY:
            handle_show_history(session);
            break;
        case CLI_GROUP_ID_SHOW_CONFIG:
            handle_show_config(session);
            break;
        case CLI_GROUP_ID_EXIT:
            handle_op_exit(session);
            break;
        case CLI_GROUP_ID_CONFIG:
            handle_op_config(session);
            break;
        case CLI_GROUP_ID_END:
            handle_op_end(session);
            break;
        case CLI_GROUP_ID_BASH:
            handle_op_bash(session);
            break;
        case CLI_GROUP_ID_SHOW_CONTEXT:
            handle_show_context(session);
            break;
        case CLI_GROUP_ID_TERMINAL_LENGTH_ZERO:
            handle_terminal_length_zero(session);
            break;
        default:
            LOG_WARN("Unknown group_id: %u", parser.group_id);
            cli_send_message(session, "Error: Unknown CFG command.\r\n");
            cli_tlv_cleanup(&parser);
            return ERRCODE_FAIL;
    }

    cli_tlv_cleanup(&parser);
    return ERRCODE_SUCCESS;
}
