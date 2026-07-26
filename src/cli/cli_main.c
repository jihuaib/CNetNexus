/**
 * @file   cli_main.c
 * @brief  CLI 模块主入口，三阶段初始化
 * @author jhb
 * @date   2026/01/22
 */
#include "cli_main.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access.h"
#include "cli.h"
#include "cli_cfg.h"
#include "cli_handler.h"
#include "cli_restore.h"
#include "cli_xml_parser.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "path_utils.h"
#include "syslog_report.h"

cli_local_t *g_cli_local = NULL;

// ============================================================================
// 三阶段回调辅助函数
// ============================================================================

// ============================================================================
// 自动发现并加载 commands.xml
// ============================================================================

/**
 * @brief 扫描指定目录下所有子目录的 resources/commands.xml 并加载到视图树
 * @param base_dir 基础目录路径
 * @return 成功加载的 XML 数量
 */
static int cli_scan_and_load_xml(const char *base_dir)
{
    struct dirent **entries = NULL;
    int count = scandir(base_dir, &entries, NULL, alphasort);
    if (count < 0)
    {
        return 0;
    }

    GPtrArray *xml_paths = g_ptr_array_new_with_free_func(g_free);
    if (!xml_paths)
    {
        for (int i = 0; i < count; i++)
        {
            free(entries[i]);
        }
        free(entries);
        return 0;
    }

    for (int i = 0; i < count; i++)
    {
        struct dirent *entry = entries[i];
        if (!entry)
        {
            continue;
        }

        if (entry->d_name[0] == '.')
        {
            free(entry);
            continue;
        }

        /* 兼容两种布局：
         *   dev  布局: {base_dir}/{module}/resources/commands.xml  (源码树)
         *   prod 布局: {base_dir}/{module}/commands.xml            (部署包) */
        struct stat st;
        char *xml_path = g_build_filename(base_dir, entry->d_name, "resources", "commands.xml", NULL);
        if (stat(xml_path, &st) != 0)
        {
            g_free(xml_path);
            xml_path = g_build_filename(base_dir, entry->d_name, "commands.xml", NULL);
            if (stat(xml_path, &st) != 0)
            {
                g_free(xml_path);
                free(entry);
                continue;
            }
        }

        g_ptr_array_add(xml_paths, xml_path);
        free(entry);
    }

    free(entries);

    /* 两阶段加载，避免模块间 view 依赖受目录扫描顺序影响。 */
    int loaded = 0;
    for (guint i = 0; i < xml_paths->len; i++)
    {
        const char *xml_path = g_ptr_array_index(xml_paths, i);
        LOG_INFO("Found XML: %s", xml_path);
        if (cli_xml_load_view_tree_ex(xml_path, &g_cli_local->view_tree, CLI_XML_LOAD_VIEWS) == ERRCODE_SUCCESS)
        {
            loaded++;
        }
        else
        {
            LOG_ERROR("Failed to load XML views: %s", xml_path);
        }
    }

    for (guint i = 0; i < xml_paths->len; i++)
    {
        const char *xml_path = g_ptr_array_index(xml_paths, i);
        if (cli_xml_load_view_tree_ex(xml_path, &g_cli_local->view_tree, CLI_XML_LOAD_COMMANDS) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("Failed to load XML commands: %s", xml_path);
        }
    }

    g_ptr_array_free(xml_paths, TRUE);
    return loaded;
}

/**
 * @brief 自动发现并加载所有模块的 commands.xml
 *
 * 按以下优先级扫描目录：
 * 1. 环境变量 NN_WORK_DIR（取 resources 子目录）
 * 2. 相对于可执行文件的开发路径
 */
static void cli_discover_and_load_xml(void)
{
    LOG_INFO("Auto-discovering and loading commands.xml...");

    int total = 0;

    /* 优先级 1: 环境变量 NN_WORK_DIR（生产环境） */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        char *resources_dir = g_build_filename(work_dir, "resources", NULL);
        total = cli_scan_and_load_xml(resources_dir);
        g_free(resources_dir);
        if (total > 0)
        {
            LOG_INFO("Loaded %d XML configs from NN_WORK_DIR", total);
            return;
        }
    }

    /* 优先级 2: 相对于可执行文件的开发路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        char *dev_path = g_build_filename(exe_dir, "..", "..", "src", NULL);
        total = cli_scan_and_load_xml(dev_path);
        g_free(dev_path);
        if (total > 0)
        {
            LOG_INFO("Loaded %d XML configs from dev path", total);
            return;
        }
    }

    LOG_ERROR("No commands.xml found");
}

// ============================================================================
// 本地状态初始化（从 constructor 调用）
// ============================================================================

void cli_init_local(dev_ipc_context_t *ctx)
{
    g_cli_local = g_malloc0(sizeof(cli_local_t));
    pthread_mutex_init(&g_cli_local->history_mutex, NULL);
    g_cli_local->running = 0;
    g_cli_local->epoll_fd = DEV_INVALID_FD;
    g_cli_local->listen_sock = DEV_INVALID_FD;
    g_cli_local->worker_thread = 0;
    g_cli_local->dev_ipc_ctx = ctx;
    g_strlcpy(g_cli_local->sysname, CLI_SYSNAME_DEFAULT, sizeof(g_cli_local->sysname));
    g_cli_local->sessions = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, (GDestroyNotify)cli_session_destroy);

    /* 创建视图树 */
    cli_view_node_t *user_view = cli_view_create(CLI_VIEW_USER, "<NetNexus>");
    if (!user_view)
    {
        LOG_ERROR("Failed to create user view");
        return;
    }
    g_cli_local->view_tree.root = user_view;

    cli_view_node_t *config_view = cli_view_create(CLI_VIEW_CONFIG, "<NetNexus(config)>");
    if (!config_view)
    {
        LOG_ERROR("Failed to create config view");
        return;
    }
    cli_view_add_child(user_view, config_view);

    /* 自动发现并加载所有模块的 commands.xml */
    cli_discover_and_load_xml();

    LOG_INFO("Local state initialization complete");
}

// ============================================================================
// ACCESS（line 层）RPC 处理
// ============================================================================

/** 连接建立后发给 ACCESS 的欢迎语 */
#define CLI_WELCOME_TEXT "\r\nWelcome to NetNexus CLI\r\nType '?' for available commands\r\n\r\n"
/** ACCESS 文本响应正文分片大小；保持远小于 DEV_IPC_RECV_BUF_SIZE，和业务模块 CLI 分片一致。 */
#define CLI_ACCESS_TEXT_CHUNK_MAX (CLI_MAX_RESP_LEN - 1)

/**
 * @brief 建立一条逻辑会话（以 ACCESS 线号为键）
 *
 * ACCESS 架构下 CLI 不持有 socket：client_fd 恒为 -1，命令输出累积到 out 缓冲。
 */
static cli_session_t *cli_logical_session_create(uint32_t line_id, const char *ip, uint16_t port)
{
    cli_session_t *session = g_malloc0(sizeof(cli_session_t));
    session->line_id = line_id;
    session->current_view = g_cli_local->view_tree.root;
    session->out = g_string_new("");
    session->close_requested = 0;
    g_strlcpy(session->client_ip, ip ? ip : "unknown", sizeof(session->client_ip));
    session->client_port = port;
    session->connect_time = time(NULL);
    update_prompt_from_template(session, session->current_view->prompt_template);

    guint *key = g_malloc(sizeof(guint));
    *key = line_id;
    g_hash_table_insert(g_cli_local->sessions, key, session);
    return session;
}

static void cli_access_output_reset(cli_session_t *session)
{
    if (!session)
    {
        return;
    }
    if (session->access_out_pending)
    {
        g_string_free(session->access_out_pending, TRUE);
        session->access_out_pending = NULL;
    }
    session->access_out_offset = 0;
}

/** @brief 构造并发送 access_text_resp_t 响应（OPEN_RESP / INPUT_RESP / CLOSE_RESP） */
static int cli_send_text_resp_len(dev_ipc_context_t *ctx, dev_ipc_message_t *req, uint32_t msg_type, uint32_t flags,
                                  const cli_session_t *line_src, const char *prompt, const char *text, size_t text_len)
{
    size_t payload_len = sizeof(access_text_resp_t) + text_len + 1;
    access_text_resp_t *r = g_malloc0(payload_len);
    r->flags = flags;
    if (line_src)
    {
        r->line_cmd = line_src->line_cmd;
        r->line_cmd_no = line_src->line_cmd_no;
        r->line_arg1 = line_src->line_cmd_arg1;
        r->line_arg2 = line_src->line_cmd_arg2;
    }
    if (prompt)
    {
        g_strlcpy(r->prompt, prompt, sizeof(r->prompt));
    }
    if (text && text_len > 0)
    {
        memcpy(r->text, text, text_len);
    }
    r->text[text_len] = '\0';

    dev_ipc_message_t *resp = dev_ipc_message_create(msg_type, DEV_MODULE_ID_CLI, req->src_module_id, req->request_id,
                                                     r, payload_len, g_free);
    if (resp)
    {
        int ret = dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
        return (ret == ERRCODE_SUCCESS) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    g_free(r);
    return ERRCODE_FAIL;
}

static int cli_send_text_resp(dev_ipc_context_t *ctx, dev_ipc_message_t *req, uint32_t msg_type, uint32_t flags,
                              const cli_session_t *line_src, const char *prompt, const char *text)
{
    size_t text_len = text ? strlen(text) : 0;
    return cli_send_text_resp_len(ctx, req, msg_type, flags, line_src, prompt, text, text_len);
}

static int cli_send_input_resp_next_chunk(dev_ipc_context_t *ctx, dev_ipc_message_t *req, cli_session_t *session)
{
    if (!session || !session->access_out_pending)
    {
        return cli_send_text_resp(ctx, req, ACCESS_MSG_INPUT_RESP, 0, NULL, "", "");
    }

    gsize full_len = session->access_out_pending->len;
    if (session->access_out_offset >= full_len)
    {
        cli_access_output_reset(session);
        return cli_send_text_resp(ctx, req, ACCESS_MSG_INPUT_RESP, 0, NULL, "", "");
    }

    gsize remaining = full_len - session->access_out_offset;
    gsize chunk_len = remaining > CLI_ACCESS_TEXT_CHUNK_MAX ? CLI_ACCESS_TEXT_CHUNK_MAX : remaining;
    gsize next_offset = session->access_out_offset + chunk_len;
    int has_more = (next_offset < full_len) ? 1 : 0;

    char prompt[ACCESS_PROMPT_MAX_LEN] = "";
    uint32_t flags = has_more ? ACCESS_RESP_FLAG_MORE : (session->close_requested ? ACCESS_RESP_FLAG_CLOSE_SESSION : 0);
    const cli_session_t *line_src = NULL;
    if (!has_more)
    {
        cli_render_prompt(session, prompt, sizeof(prompt));
        line_src = session;
    }

    int ret = cli_send_text_resp_len(ctx, req, ACCESS_MSG_INPUT_RESP, flags, line_src, prompt,
                                     session->access_out_pending->str + session->access_out_offset, chunk_len);
    if (ret == ERRCODE_SUCCESS)
    {
        session->access_out_offset = next_offset;
        if (!has_more)
        {
            cli_access_output_reset(session);
        }
    }
    return ret;
}

static int cli_send_input_resp_chunked(dev_ipc_context_t *ctx, dev_ipc_message_t *req, uint32_t flags,
                                       cli_session_t *session, const char *prompt, const char *text)
{
    if (!session)
    {
        return cli_send_text_resp(ctx, req, ACCESS_MSG_INPUT_RESP, flags, NULL, prompt, text);
    }

    cli_access_output_reset(session);

    size_t text_len = text ? strlen(text) : 0;
    if (text_len <= CLI_ACCESS_TEXT_CHUNK_MAX)
    {
        return cli_send_text_resp_len(ctx, req, ACCESS_MSG_INPUT_RESP, flags, session, prompt, text, text_len);
    }

    session->access_out_pending = g_string_new_len(text, (gssize)text_len);
    session->access_out_offset = 0;
    return cli_send_input_resp_next_chunk(ctx, req, session);
}

static void cli_notify_line_closed(uint32_t line_id)
{
    if (!g_cli_local || !g_cli_local->dev_ipc_ctx)
    {
        return;
    }

    uint32_t *payload = g_new(uint32_t, 1);
    *payload = line_id;
    dev_ipc_message_t *notify = dev_ipc_message_create(CLI_MSG_TYPE_LINE_CLOSED, DEV_MODULE_ID_CLI, DEV_MODULE_ID_DEV,
                                                       0, payload, sizeof(*payload), g_free);
    if (!notify)
    {
        g_free(payload);
        return;
    }

    if (dev_ipc_send(g_cli_local->dev_ipc_ctx, DEV_MODULE_ID_DEV, notify) != ERRCODE_SUCCESS)
    {
        LOG_WARN("CLI: failed to notify DEV that line %u closed", line_id);
    }
    dev_ipc_message_free(notify);
}

/** @brief 处理 ACCESS_MSG_SESSION_OPEN：建逻辑会话，回 welcome + 初始提示符 */
static void cli_handle_session_open(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(access_session_open_t))
    {
        cli_send_text_resp(ctx, msg, ACCESS_MSG_OPEN_RESP, 0, NULL, "<NetNexus>", "");
        return;
    }
    access_session_open_t *req = (access_session_open_t *)msg->payload;
    cli_session_t *s = cli_logical_session_create(req->line_id, req->client_ip, req->client_port);
    char prompt[ACCESS_PROMPT_MAX_LEN];
    cli_render_prompt(s, prompt, sizeof(prompt));
    LOG_INFO("CLI: line %u session opened (peer=%s)", req->line_id, req->client_ip);
    cli_send_text_resp(ctx, msg, ACCESS_MSG_OPEN_RESP, 0, NULL, prompt, CLI_WELCOME_TEXT);
}

/** @brief 处理 ACCESS_MSG_LINE_INPUT：执行命令，回输出文本 + 新提示符 + close/line 命令标志 */
static void cli_handle_line_input(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    access_line_input_t *req = (access_line_input_t *)msg->payload;
    if (!req || msg->payload_len <= sizeof(access_line_input_t))
    {
        cli_send_text_resp(ctx, msg, ACCESS_MSG_INPUT_RESP, 0, NULL, "<NetNexus>", "");
        return;
    }

    cli_session_t *s = g_hash_table_lookup(g_cli_local->sessions, &req->line_id);
    if (!s)
    {
        /* 会话不存在（CLI 重启后丢失）：要求 ACCESS 关闭该线，触发其重新 OPEN。 */
        cli_send_text_resp(ctx, msg, ACCESS_MSG_INPUT_RESP, ACCESS_RESP_FLAG_CLOSE_SESSION, NULL, "", "");
        return;
    }

    /*
     * CLI 必须先 notify READY，startup/cfg 内部回放才能等待 DEV 的整体 READY；
     * 因而 ACCESS 可能在回放结束前已连入。此窗口内拒绝外部命令，防止 show/
     * 配置命令与内部 session 并发，读到半份配置或抢先 auto-start 同一模块。
     */
    if (!g_atomic_int_get(&g_cli_local->startup_restore_complete))
    {
        char prompt[ACCESS_PROMPT_MAX_LEN];
        cli_render_prompt(s, prompt, sizeof(prompt));
        cli_send_text_resp(ctx, msg, ACCESS_MSG_INPUT_RESP, 0, s, prompt,
                           "CLI: startup configuration replay in progress; retry.\r\n");
        return;
    }

    s->line_cmd = 0;
    s->line_cmd_no = 0;
    s->line_cmd_arg1 = 0;
    s->line_cmd_arg2 = 0;
    cli_access_output_reset(s);
    g_string_truncate(s->out, 0);
    if (req->cmdline[0] != '\0')
    {
        const char *view = s->current_view ? s->current_view->view_name : "unknown";
        syslog_report(SYSLOG_REPORT_NOTICE, "cli", "command", "line=%u peer=%s view=%s cmd=\"%s\"",
                      (unsigned)s->line_id, s->client_ip, view, req->cmdline);
    }
    process_command(req->cmdline, s);

    if (req->cmdline[0] != '\0')
    {
        pthread_mutex_lock(&g_cli_local->history_mutex);
        cli_global_history_add(&g_cli_local->global_history, req->cmdline, s->client_ip);
        pthread_mutex_unlock(&g_cli_local->history_mutex);
    }

    char prompt[ACCESS_PROMPT_MAX_LEN];
    cli_render_prompt(s, prompt, sizeof(prompt));
    uint32_t flags = s->close_requested ? ACCESS_RESP_FLAG_CLOSE_SESSION : 0;
    (void)cli_send_input_resp_chunked(ctx, msg, flags, s, prompt, s->out->str);
}

/** @brief 处理 ACCESS_MSG_INPUT_CONTINUE：继续回传上一条 LINE_INPUT 的文本分片 */
static void cli_handle_input_continue(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(uint32_t))
    {
        cli_send_text_resp(ctx, msg, ACCESS_MSG_INPUT_RESP, 0, NULL, "", "");
        return;
    }

    uint32_t line_id = 0;
    memcpy(&line_id, msg->payload, sizeof(line_id));
    cli_session_t *s = g_hash_table_lookup(g_cli_local->sessions, &line_id);
    if (!s)
    {
        cli_send_text_resp(ctx, msg, ACCESS_MSG_INPUT_RESP, ACCESS_RESP_FLAG_CLOSE_SESSION, NULL, "", "");
        return;
    }

    (void)cli_send_input_resp_next_chunk(ctx, msg, s);
}

/** @brief 处理 ACCESS_MSG_SESSION_CLOSE：销毁逻辑会话 */
static void cli_handle_session_close(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (msg->payload && msg->payload_len >= sizeof(uint32_t))
    {
        uint32_t line_id = *(uint32_t *)msg->payload;
        gboolean removed = g_hash_table_remove(g_cli_local->sessions, &line_id);
        cli_notify_line_closed(line_id);
        LOG_INFO("CLI: line %u session closed", line_id);
        if (!removed)
        {
            LOG_WARN("CLI: line %u session close had no logical session", line_id);
        }
    }
    cli_send_text_resp(ctx, msg, ACCESS_MSG_CLOSE_RESP, 0, NULL, "", "");
}

/** @brief 处理 ACCESS_MSG_TAB_REQ：用命令树算候选 token 列表回传 */
static void cli_handle_tab_req(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    access_line_input_t *req = (access_line_input_t *)msg->payload;
    GString *toks = g_string_new("");
    if (req && msg->payload_len >= sizeof(access_line_input_t))
    {
        cli_session_t *s = g_hash_table_lookup(g_cli_local->sessions, &req->line_id);
        if (s)
        {
            cli_build_tab_candidates(s, req->cmdline, toks);
        }
    }

    size_t payload_len = sizeof(access_tab_resp_t) + toks->len;
    access_tab_resp_t *r = g_malloc0(payload_len);
    r->kind = (toks->len > 0) ? ACCESS_TAB_KIND_TOKENS : ACCESS_TAB_KIND_NONE;
    if (toks->len > 0)
    {
        memcpy(r->data, toks->str, toks->len);
    }
    g_string_free(toks, TRUE);

    dev_ipc_message_t *resp = dev_ipc_message_create(ACCESS_MSG_TAB_RESP, DEV_MODULE_ID_CLI, msg->src_module_id,
                                                     msg->request_id, r, payload_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(r);
    }
}

/** @brief 处理 ACCESS_MSG_HELP_REQ：用命令树构建帮助文本回传 */
static void cli_handle_help_req(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    access_line_input_t *req = (access_line_input_t *)msg->payload;
    GString *h = g_string_new("");
    if (req && msg->payload_len >= sizeof(access_line_input_t))
    {
        cli_session_t *s = g_hash_table_lookup(g_cli_local->sessions, &req->line_id);
        if (s)
        {
            cli_build_help_text(s, req->cmdline, h);
        }
    }

    size_t payload_len = h->len + 1;
    char *text = g_malloc(payload_len);
    memcpy(text, h->str, h->len);
    text[h->len] = '\0';
    g_string_free(h, TRUE);

    dev_ipc_message_t *resp = dev_ipc_message_create(ACCESS_MSG_HELP_RESP, DEV_MODULE_ID_CLI, msg->src_module_id,
                                                     msg->request_id, text, payload_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(text);
    }
}

/** @brief 处理 DB 内部 RPC：导出 show current-configuration 的 BDR 文本 */
static void cli_handle_export_config(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    GString *out = cli_cfg_collect_current_config(DEV_MODULE_ID_DB);
    (void)cli_chunk_stream_start(&g_cli_local->export_stream, ctx, DEV_MODULE_ID_CLI, msg, out);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void cli_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    switch (msg->msg_type)
    {
        case ACCESS_MSG_SESSION_OPEN:
            cli_handle_session_open(ctx, msg);
            break;

        case ACCESS_MSG_LINE_INPUT:
            cli_handle_line_input(ctx, msg);
            break;

        case ACCESS_MSG_INPUT_CONTINUE:
            cli_handle_input_continue(ctx, msg);
            break;

        case ACCESS_MSG_SESSION_CLOSE:
            cli_handle_session_close(ctx, msg);
            break;

        case ACCESS_MSG_TAB_REQ:
            cli_handle_tab_req(ctx, msg);
            break;

        case ACCESS_MSG_HELP_REQ:
            cli_handle_help_req(ctx, msg);
            break;

        case CLI_MSG_TYPE_EXPORT_CONFIG:
            cli_handle_export_config(ctx, msg);
            break;

        case CLI_MSG_TYPE_CONTINUE:
            (void)cli_chunk_stream_continue(&g_cli_local->export_stream, ctx, DEV_MODULE_ID_CLI, msg);
            break;

        case CLI_MSG_TYPE_SYSNAME_UPDATE:
        {
            const char *new_name = (msg->payload && msg->payload_len > 0) ? (const char *)msg->payload : "";
            if (new_name[0] == '\0')
            {
                g_strlcpy(g_cli_local->sysname, CLI_SYSNAME_DEFAULT, sizeof(g_cli_local->sysname));
            }
            else
            {
                g_strlcpy(g_cli_local->sysname, new_name, sizeof(g_cli_local->sysname));
            }
            LOG_INFO("CLI: sysname updated to '%s'", g_cli_local->sysname);
            break;
        }

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================

#include "cli_main.h"

int cli_module_init(void)
{
    log_set_tag("cli");
    LOG_INFO("Module initialization");

    /* 创建 IPC 上下文 */
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_CLI, "cli", DEV_MODULE_PORT_CLI, cli_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    /* 初始化本地状态（view tree、sessions 等；不含 telnet 监听） */
    cli_init_local(ctx);

    /* 弱依赖模型 init：
     *   1. 等 DEV 控制连接
     *   2. 启动 Telnet server（业务模块需要它来收命令）
     *   3. notify_ready
     * CLI 不订阅其它模块（业务模块 init 末尾会反向 subscribe(CLI)）。 */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("CLI: timed out waiting for DEV connection");
    }

    /* Telnet 接入由 ACCESS 模块（line 层）承担，CLI 不再监听固定业务端口，
     * 仅作为命令引擎按 line_id 处理 ACCESS 转发来的会话/命令 RPC。 */

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("CLI: notify_ready to DEV failed");
    }
    LOG_INFO("CLI: module ready");

    cli_restore_startup_if_needed();
    g_atomic_int_set(&g_cli_local->startup_restore_complete, 1);

    return 0;
}

void cli_module_cleanup(void)
{
    if (!g_cli_local)
    {
        return;
    }

    /* 必须先停掉 server thread 再释放它会读到的状态（视图、session、fd）。
     * 否则 T3 仍在 epoll_wait / process_command 中触碰 current_view，
     * 而主线程已经走到 cli_cleanup() 释放视图，出现 heap-use-after-free。 */
    g_cli_local->running = 0;
    if (g_cli_local->worker_thread != 0)
    {
        pthread_join(g_cli_local->worker_thread, NULL);
        g_cli_local->worker_thread = 0;
    }

    /* server thread 已退出，可以安全销毁 IPC（IPC 线程不会再触发依赖 CLI 状态的回调）。 */
    dev_ipc_context_t *ctx = g_cli_local->dev_ipc_ctx;

    /* 向 DEV 发 PRE_EXIT 通知，等 DEV 同步完成 phase/broadcast/drop 后再 ACK。
     * 必须在 dev_ipc_destroy 之前；超时/失败不阻塞退出，SIGCHLD 路径仍会兜底清理。 */
    if (ctx)
    {
        dev_ipc_pre_exit_notify(ctx, 3000);
    }

    g_cli_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    cli_chunk_stream_reset(&g_cli_local->export_stream);

    if (g_cli_local->listen_sock != DEV_INVALID_FD)
    {
        close(g_cli_local->listen_sock);
        g_cli_local->listen_sock = DEV_INVALID_FD;
    }

    if (g_cli_local->epoll_fd != DEV_INVALID_FD)
    {
        close(g_cli_local->epoll_fd);
        g_cli_local->epoll_fd = DEV_INVALID_FD;
    }

    /* 先销毁 session（其 current_view 是借用指针），再释放视图树。 */
    if (g_cli_local->sessions != NULL)
    {
        g_hash_table_destroy(g_cli_local->sessions);
        g_cli_local->sessions = NULL;
    }

    cli_cleanup();

    cli_global_history_cleanup(&g_cli_local->global_history);
    pthread_mutex_destroy(&g_cli_local->history_mutex);

    g_free(g_cli_local);
    g_cli_local = NULL;
}
