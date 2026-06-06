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
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli.h"
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

static dev_ipc_message_t *create_show_config_request(uint32_t mod_id, const uint8_t *scope_payload,
                                                     uint32_t scope_payload_len)
{
    uint8_t *payload_copy = NULL;
    if (scope_payload && scope_payload_len > 0)
    {
        payload_copy = g_memdup2(scope_payload, scope_payload_len);
        if (!payload_copy)
        {
            return NULL;
        }
    }

    dev_ipc_message_t *req = dev_ipc_message_create(CLI_MSG_TYPE_SHOW_CONFIG, DEV_MODULE_ID_CLI, mod_id, 0,
                                                    payload_copy, scope_payload_len, payload_copy ? g_free : NULL);
    if (!req)
    {
        g_free(payload_copy);
    }
    return req;
}

/**
 * @brief 拉取单个模块 SHOW_CONFIG 的完整输出（支持 RESP_MORE/CONTINUE）
 *        output 在调用前会被清空, 函数返回后仅含本模块的完整响应文本。
 */
static void collect_module_show_config(uint32_t mod_id, const uint8_t *scope_payload, uint32_t scope_payload_len,
                                       GString *output)
{
    if (output)
    {
        g_string_truncate(output, 0);
    }
    dev_ipc_message_t *req = create_show_config_request(mod_id, scope_payload, scope_payload_len);
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
            LOG_WARN("SHOW_CONFIG: module 0x%08X query timeout", mod_id);
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
            LOG_WARN("SHOW_CONFIG: module 0x%08X returned unexpected msg_type=0x%08X", mod_id, resp->msg_type);
        }

        dev_ipc_message_free(resp);
    }

    if (req)
    {
        dev_ipc_message_free(req);
    }

    if (chunks >= max_chunks)
    {
        LOG_WARN("SHOW_CONFIG: module 0x%08X exceeded max chunks(%u), stop collecting", mod_id, max_chunks);
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

static gboolean is_show_config_module_connected(uint32_t mod_id)
{
    if (!g_cli_local || !g_cli_local->dev_ipc_ctx)
    {
        return FALSE;
    }

    gboolean connected = FALSE;
    dev_ipc_context_t *ctx = g_cli_local->dev_ipc_ctx;
    pthread_mutex_lock(&ctx->comutex);
    for (int i = 0; i < ctx->num_connections; i++)
    {
        dev_ipc_connection_t *conn = ctx->connections[i];
        if (conn && conn->state == DEV_IPC_COCONNECTED && conn->remote_module_id == mod_id)
        {
            connected = TRUE;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->comutex);
    return connected;
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

GString *cli_cfg_collect_current_config(uint32_t exclude_module_id)
{
    cli_cfg_anchor_aggregator_t *agg = cli_cfg_anchor_agg_new();
    GString *buf = g_string_new("");
    GString *output = g_string_new("");

    GArray *modules = collect_connected_modules_for_show_config();
    for (guint i = 0; i < modules->len; i++)
    {
        uint32_t mod_id = g_array_index(modules, uint32_t, i);
        if (exclude_module_id != 0 && mod_id == exclude_module_id)
        {
            continue;
        }

        /* 先完整拉取当前模块的响应再喂给聚合器, 再切下一个模块 */
        collect_module_show_config(mod_id, NULL, 0, buf);
        if (buf->len > 0)
        {
            cli_cfg_anchor_agg_feed(agg, buf->str);
        }
    }
    g_array_free(modules, TRUE);
    g_string_free(buf, TRUE);
    cli_cfg_anchor_agg_render(agg, output);
    cli_cfg_anchor_agg_free(agg);
    return output;
}

static void render_show_config_output(cli_session_t *session, GString *output)
{
    if (output && output->len > 0)
    {
        cli_pager_output(session, output->str);
    }
    else
    {
        cli_send_message(session, "No configuration found.\r\n");
    }
}

#define CLI_CFG_DIFF_PARAM_FILE 1

typedef struct cli_cfg_diff_line
{
    char *key;
    char *display;
} cli_cfg_diff_line_t;

typedef struct cli_cfg_diff_config
{
    GHashTable *seen;
    GPtrArray *lines;
} cli_cfg_diff_config_t;

static void cli_cfg_diff_line_free(gpointer data)
{
    cli_cfg_diff_line_t *line = (cli_cfg_diff_line_t *)data;
    if (!line)
    {
        return;
    }

    g_free(line->key);
    g_free(line->display);
    g_free(line);
}

static cli_cfg_diff_config_t *cli_cfg_diff_config_new(void)
{
    cli_cfg_diff_config_t *cfg = g_malloc0(sizeof(*cfg));
    cfg->seen = g_hash_table_new(g_str_hash, g_str_equal);
    cfg->lines = g_ptr_array_new_with_free_func(cli_cfg_diff_line_free);
    return cfg;
}

static void cli_cfg_diff_config_free(cli_cfg_diff_config_t *cfg)
{
    if (!cfg)
    {
        return;
    }

    if (cfg->seen)
    {
        g_hash_table_destroy(cfg->seen);
    }
    if (cfg->lines)
    {
        g_ptr_array_free(cfg->lines, TRUE);
    }
    g_free(cfg);
}

static void cli_cfg_diff_config_add_line(cli_cfg_diff_config_t *cfg, const char *line)
{
    if (!cfg || !line)
    {
        return;
    }

    char *display = g_strdup(line);
    g_strchomp(display);

    char *key = g_strdup(display);
    g_strstrip(key);

    if (key[0] == '\0' || strcmp(key, "!") == 0 || g_hash_table_contains(cfg->seen, key))
    {
        g_free(key);
        g_free(display);
        return;
    }

    cli_cfg_diff_line_t *item = g_malloc0(sizeof(*item));
    item->key = key;
    item->display = display;
    g_hash_table_add(cfg->seen, item->key);
    g_ptr_array_add(cfg->lines, item);
}

static cli_cfg_diff_config_t *cli_cfg_diff_config_from_text(const char *text)
{
    cli_cfg_diff_config_t *cfg = cli_cfg_diff_config_new();
    if (!text || text[0] == '\0')
    {
        return cfg;
    }

    gchar **lines = g_strsplit(text, "\n", -1);
    for (guint i = 0; lines && lines[i]; i++)
    {
        cli_cfg_diff_config_add_line(cfg, lines[i]);
    }
    g_strfreev(lines);
    return cfg;
}

static GString *cli_cfg_diff_build_text(const char *current_text, const char *target_text)
{
    cli_cfg_diff_config_t *current = cli_cfg_diff_config_from_text(current_text);
    cli_cfg_diff_config_t *target = cli_cfg_diff_config_from_text(target_text);
    GString *out = g_string_new("");

    for (guint i = 0; i < target->lines->len; i++)
    {
        cli_cfg_diff_line_t *line = g_ptr_array_index(target->lines, i);
        if (!g_hash_table_contains(current->seen, line->key))
        {
            g_string_append_printf(out, "+%s\r\n", line->display);
        }
    }

    for (guint i = 0; i < current->lines->len; i++)
    {
        cli_cfg_diff_line_t *line = g_ptr_array_index(current->lines, i);
        if (!g_hash_table_contains(target->seen, line->key))
        {
            g_string_append_printf(out, "-%s\r\n", line->display);
        }
    }

    cli_cfg_diff_config_free(current);
    cli_cfg_diff_config_free(target);
    return out;
}

static char *cli_cfg_diff_resolve_cfg_path(const char *arg)
{
    if (!arg || arg[0] == '\0')
    {
        return NULL;
    }

    if (access(arg, R_OK) == 0 || g_path_is_absolute(arg) || strchr(arg, '/'))
    {
        return g_strdup(arg);
    }

    const char *work_dir = getenv("NN_WORK_DIR");
    char *config_dir = (work_dir && work_dir[0] != '\0') ? g_build_filename(work_dir, "data", "configs", NULL)
                                                         : g_build_filename(".", "data", "configs", NULL);

    char *path = g_build_filename(config_dir, arg, NULL);
    if (access(path, R_OK) == 0)
    {
        g_free(config_dir);
        return path;
    }
    g_free(path);

    if (!g_str_has_suffix(arg, ".cfg"))
    {
        char *with_suffix = g_strdup_printf("%s.cfg", arg);
        path = g_build_filename(config_dir, with_suffix, NULL);
        g_free(with_suffix);
        if (access(path, R_OK) == 0)
        {
            g_free(config_dir);
            return path;
        }
        g_free(path);
    }

    g_free(config_dir);
    return g_strdup(arg);
}

/**
 * @brief show configuration difference current-configuration <configuration-file> (group_id=12)
 */
static void handle_show_conf_diff(cli_session_t *session, cli_tlv_parser_t *parser)
{
    char *cfg_file = NULL;

    cli_tlv_entry_t entry;
    int rc = 0;
    while ((rc = cli_tlv_next(parser, &entry)) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == CLI_CFG_DIFF_PARAM_FILE)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text && text[0] != '\0')
            {
                g_free(cfg_file);
                cfg_file = g_strdup(text);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (rc < 0)
    {
        cli_send_message(session, "Error: Invalid command parameters.\r\n");
        g_free(cfg_file);
        return;
    }

    if (!cfg_file)
    {
        cli_send_message(session, "Error: Missing configuration file.\r\n");
        return;
    }

    char *path = cli_cfg_diff_resolve_cfg_path(cfg_file);
    gchar *target_text = NULL;
    GError *gerr = NULL;
    if (!path || !g_file_get_contents(path, &target_text, NULL, &gerr))
    {
        char *msg = g_strdup_printf("Error: Unable to read configuration file '%s': %s.\r\n", path ? path : cfg_file,
                                    gerr ? gerr->message : "read failed");
        cli_send_message(session, msg);
        g_free(msg);
        if (gerr)
        {
            g_error_free(gerr);
        }
        g_free(path);
        g_free(cfg_file);
        return;
    }

    GString *current = cli_cfg_collect_current_config(0);
    GString *diff = cli_cfg_diff_build_text(current ? current->str : "", target_text ? target_text : "");
    if (diff->len > 0)
    {
        cli_pager_output(session, diff->str);
    }
    else
    {
        cli_send_message(session, "No configuration difference.\r\n");
    }

    g_string_free(diff, TRUE);
    if (current)
    {
        g_string_free(current, TRUE);
    }
    g_free(target_text);
    g_free(path);
    g_free(cfg_file);
}

/**
 * @brief show current-configuration (group_id=3)
 *
 * 向所有业务模块发送 CLI_MSG_TYPE_SHOW_CONFIG，收集响应后交由通用 config-anchor
 * 聚合器合并: 同一 anchor key 的贡献在最终输出中仅以一个段落呈现,
 * CLI 本身对 anchor key 的含义完全不感知(例如接口、VRF 等全部走同一机制)。
 * not connected的模块直接跳过，避免超时等待。
 */
static void handle_show_config(cli_session_t *session)
{
    GString *output = cli_cfg_collect_current_config(0);
    render_show_config_output(session, output);
    g_string_free(output, TRUE);
}

/**
 * @brief show this (group_id=10)
 *
 * 当前视图是否支持 scoped show-this 以及应查询哪些模块，均由各模块 XML 的
 * <show-this><view name="..."/></show-this> 元数据声明，不再在 CLI 中硬编码白名单。
 * 未声明 scoped 支持的视图仍回退到 show current-configuration。
 */
static void handle_show_this(cli_session_t *session)
{
    if (!session || !session->current_view)
    {
        cli_send_message(session, "Error: No active view.\r\n");
        return;
    }

    const char *view_name = session->current_view->view_name;
    if (!cli_view_supports_show_this(session->current_view))
    {
        handle_show_config(session);
        return;
    }

    uint32_t ctx_len = 0;
    const uint8_t *ctx_data = cli_context_get(session, &ctx_len);

    cli_show_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.mode = CLI_SHOW_SCOPE_MODE_THIS;
    strlcpy(scope.view_name, view_name, sizeof(scope.view_name));
    scope.ctx_data = ctx_data;
    scope.ctx_len = ctx_len;

    uint32_t payload_len = 0;
    uint8_t *payload = cli_show_scope_payload_build(&scope, &payload_len);
    cli_cfg_anchor_aggregator_t *agg = cli_cfg_anchor_agg_new();
    GString *buf = g_string_new("");

    uint32_t num_mod_ids = 0;
    const uint32_t *mod_ids = cli_view_get_show_this_modules(session->current_view, &num_mod_ids);
    for (uint32_t i = 0; i < num_mod_ids; i++)
    {
        uint32_t mod_id = mod_ids[i];
        if (!is_show_config_module_connected(mod_id))
        {
            continue;
        }

        collect_module_show_config(mod_id, payload, payload_len, buf);
        if (buf->len > 0)
        {
            cli_cfg_anchor_agg_feed(agg, buf->str);
        }
    }

    GString *output = g_string_new("");
    cli_cfg_anchor_agg_render(agg, output);
    render_show_config_output(session, output);
    g_string_free(output, TRUE);
    g_free(payload);
    g_string_free(buf, TRUE);
    cli_cfg_anchor_agg_free(agg);
}

/**
 * @brief exit (group_id=4)
 */
static void handle_op_exit(cli_session_t *session)
{
    cli_view_node_t *parent_view = session->current_view->parent;
    if (parent_view == NULL)
    {
        /* 顶层 exit：ACCESS 架构下 CLI 不持有 socket，置关闭标志，
         * 由 LINE_INPUT 响应携带 close flag 通知 ACCESS 关连接。 */
        session->close_requested = 1;
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
 * @brief show cli client (group_id=11)：列出全部已登录 CLI 客户端
 */
typedef struct show_client_ctx
{
    GString *out;
    time_t now;
    uint32_t idx;
} show_client_ctx_t;

static void show_client_format_uptime(time_t connect_time, time_t now, char *buf, size_t cap)
{
    if (connect_time <= 0 || now < connect_time)
    {
        snprintf(buf, cap, "%s", "n/a");
        return;
    }
    long secs = (long)(now - connect_time);
    long days = secs / 86400;
    long hours = (secs % 86400) / 3600;
    long mins = (secs % 3600) / 60;
    long s = secs % 60;
    if (days > 0)
    {
        snprintf(buf, cap, "%ldd%02ldh%02ldm%02lds", days, hours, mins, s);
    }
    else if (hours > 0)
    {
        snprintf(buf, cap, "%02ldh%02ldm%02lds", hours, mins, s);
    }
    else
    {
        snprintf(buf, cap, "%02ldm%02lds", mins, s);
    }
}

static void show_client_iter(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    cli_session_t *s = (cli_session_t *)value;
    show_client_ctx_t *cx = (show_client_ctx_t *)user_data;
    if (!s)
    {
        return;
    }

    char time_str[32] = "n/a";
    if (s->connect_time > 0)
    {
        struct tm *tmv = localtime(&s->connect_time);
        if (tmv)
        {
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tmv);
        }
    }

    char up_str[32];
    show_client_format_uptime(s->connect_time, cx->now, up_str, sizeof(up_str));

    const char *view_name =
        (s->current_view && s->current_view->view_name[0] != '\0') ? s->current_view->view_name : "?";
    const char *mode = "cli";

    char line[320];
    snprintf(line, sizeof(line), "  %-3u  %-15s  %-5u  %-19s  %-13s  %-5s  %s\r\n", cx->idx, s->client_ip,
             s->client_port, time_str, up_str, mode, view_name);
    g_string_append(cx->out, line);
    cx->idx++;
}

static void handle_show_client(cli_session_t *session)
{
    GString *out = g_string_new("\r\nCLI Connected Clients:\r\n");
    g_string_append(out, "  No.  Client IP        Port   Login Time           Uptime         Mode   View\r\n");
    g_string_append(out, "  ---  ---------------  -----  -------------------  -------------  -----  --------\r\n");

    if (!g_cli_local || !g_cli_local->sessions || g_hash_table_size(g_cli_local->sessions) == 0)
    {
        g_string_append(out, "  (no active client)\r\n");
    }
    else
    {
        show_client_ctx_t cx = {.out = out, .now = time(NULL), .idx = 1};
        g_hash_table_foreach(g_cli_local->sessions, show_client_iter, &cx);
    }

    g_string_append(out, "\r\n");
    cli_pager_output(session, out->str);
    g_string_free(out, TRUE);
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
        case CLI_GROUP_ID_SHOW_CONTEXT:
            handle_show_context(session);
            break;
        case CLI_GROUP_ID_SHOW_THIS:
            handle_show_this(session);
            break;
        case CLI_GROUP_ID_SHOW_CLIENT:
            handle_show_client(session);
            break;
        case CLI_GROUP_ID_SHOW_CONF_DIFF:
            handle_show_conf_diff(session, &parser);
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
