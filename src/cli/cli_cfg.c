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
#include "cli_config_model.h"
#include "cli_config_plan.h"
#include "cli_handler.h"
#include "cli_main.h"
#include "config_capture.h"
#include "db.h"
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
static gboolean collect_module_show_config(uint32_t mod_id, const uint8_t *scope_payload, uint32_t scope_payload_len,
                                           GString *output)
{
    if (output)
    {
        g_string_truncate(output, 0);
    }
    dev_ipc_message_t *req = create_show_config_request(mod_id, scope_payload, scope_payload_len);
    if (!req)
    {
        return FALSE;
    }

    /* 保护上限：防止异常模块无限 RESP_MORE */
    const uint32_t max_chunks = 4096;
    uint32_t chunks = 0;
    gboolean complete = FALSE;

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
            else
            {
                complete = TRUE;
            }
        }
        else if (resp->msg_type == CLI_MSG_TYPE_RESP_ERROR)
        {
            const char *detail =
                (resp->payload && resp->payload_len > 1) ? (const char *)resp->payload : "unspecified module error";
            LOG_WARN("SHOW_CONFIG: module 0x%08X failed: %s", mod_id, detail);
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
        complete = FALSE;
    }
    return complete;
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
        if (conn && conn->state == DEV_IPC_COCONNECTED && !conn->draining && conn->remote_module_id == mod_id)
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
        if (!conn || conn->state != DEV_IPC_COCONNECTED || conn->draining)
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

static gboolean cli_cfg_module_array_contains(const GArray *modules, uint32_t module_id)
{
    for (guint i = 0; modules && i < modules->len; i++)
    {
        if (g_array_index((GArray *)modules, uint32_t, i) == module_id)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean cli_cfg_remote_table_has_rows(const char *table_name, gboolean *has_rows)
{
    if (!table_name || !has_rows || !g_cli_local || !g_cli_local->dev_ipc_ctx ||
        !dev_ipc_is_connected(g_cli_local->dev_ipc_ctx, DEV_MODULE_ID_DB))
    {
        return FALSE;
    }

    db_filter_builder_t filter;
    db_filter_init(&filter);
    db_filter_add_text(&filter, "type", "table");
    db_filter_add_text(&filter, "name", table_name);

    gboolean table_exists = FALSE;
    int rc = db_rpc_exists(g_cli_local->dev_ipc_ctx, "sqlite_master", &filter.filter, &table_exists);
    db_filter_clear(&filter);
    if (rc != ERRCODE_SUCCESS)
    {
        return FALSE;
    }
    if (!table_exists)
    {
        *has_rows = FALSE;
        return TRUE;
    }

    return db_rpc_exists(g_cli_local->dev_ipc_ctx, table_name, NULL, has_rows) == ERRCODE_SUCCESS;
}

/** 按逗号分隔的 revive-table 清单检查；任一表非空即视为存在配置。 */
static gboolean cli_cfg_remote_tables_have_rows(const char *table_names, gboolean *has_rows)
{
    if (!table_names || !has_rows)
    {
        return FALSE;
    }

    *has_rows = FALSE;
    gboolean saw_table = FALSE;
    gboolean ok = TRUE;
    gchar **tables = g_strsplit(table_names, ",", -1);
    for (guint i = 0; tables && tables[i]; ++i)
    {
        char *table = g_strstrip(tables[i]);
        if (table[0] == '\0')
        {
            continue;
        }
        saw_table = TRUE;

        gboolean table_has_rows = FALSE;
        if (!cli_cfg_remote_table_has_rows(table, &table_has_rows))
        {
            ok = FALSE;
            break;
        }
        if (table_has_rows)
        {
            *has_rows = TRUE;
            break;
        }
    }
    g_strfreev(tables);
    return ok && saw_table;
}

static gboolean cli_cfg_required_modules_connected(const GArray *modules, uint32_t exclude_module_id,
                                                   GHashTable *inactive_optional_modules,
                                                   gboolean inspect_optional_markers)
{
    for (guint i = 0; i < CONFIG_CAPTURE_OWNER_COUNT; i++)
    {
        const config_capture_owner_t *owner = &CONFIG_CAPTURE_OWNERS[i];
        if (owner->module_id == exclude_module_id)
        {
            continue;
        }

        gboolean required = owner->always_required;
        if (!required && !inspect_optional_markers)
        {
            /* DB->CLI 导出路径不能反向查询 DB；仍继续校验 always-required
             * owner，但 optional owner 保持 best-effort 采集。 */
            continue;
        }
        if (!required && owner->revive_table && !cli_cfg_remote_tables_have_rows(owner->revive_table, &required))
        {
            LOG_WARN("SHOW_CONFIG: cannot inspect revive table '%s' for module %s", owner->revive_table,
                     owner->module_name);
            return FALSE;
        }
        if (!required && !owner->always_required && inactive_optional_modules)
        {
            /* 按需模块的 marker 已空时，即使退出中的旧 socket 尚未 EOF，
             * 也不应再向它发 SHOW_CONFIG。marker 是是否存在可回放配置的
             * 权威判定，物理连接只用于判断 required owner 是否可采集。 */
            g_hash_table_add(inactive_optional_modules, GUINT_TO_POINTER(owner->module_id));
        }
        if (required && !cli_cfg_module_array_contains(modules, owner->module_id))
        {
            LOG_WARN("SHOW_CONFIG: required module %s (0x%08X) is not connected", owner->module_name, owner->module_id);
            return FALSE;
        }
    }
    return TRUE;
}

GString *cli_cfg_collect_current_config_checked(uint32_t exclude_module_id, gboolean *complete)
{
    if (complete)
    {
        *complete = TRUE;
    }
    cli_cfg_anchor_aggregator_t *agg = cli_cfg_anchor_agg_new();
    GString *buf = g_string_new("");
    GString *output = g_string_new("");

    GArray *modules = collect_connected_modules_for_show_config();
    GHashTable *inactive_optional_modules = g_hash_table_new(g_direct_hash, g_direct_equal);
    /* DB 请求 CLI 导出时会显式 exclude DB；该调用运行在 CLI worker，而
     * DB worker 正等导出响应，不能在这里反向查询 DB 的 revive marker。 */
    gboolean coverage_complete = cli_cfg_required_modules_connected(
        modules, exclude_module_id, inactive_optional_modules, exclude_module_id != DEV_MODULE_ID_DB);
    if (complete && !coverage_complete)
    {
        *complete = FALSE;
    }
    for (guint i = 0; i < modules->len; i++)
    {
        uint32_t mod_id = g_array_index(modules, uint32_t, i);
        if ((exclude_module_id != 0 && mod_id == exclude_module_id) ||
            g_hash_table_contains(inactive_optional_modules, GUINT_TO_POINTER(mod_id)))
        {
            continue;
        }

        /* 先完整拉取当前模块的响应再喂给聚合器, 再切下一个模块 */
        if (!collect_module_show_config(mod_id, NULL, 0, buf) && complete)
        {
            *complete = FALSE;
        }
        if (buf->len > 0)
        {
            cli_cfg_anchor_agg_feed(agg, buf->str);
        }
    }
    g_hash_table_destroy(inactive_optional_modules);
    g_array_free(modules, TRUE);
    g_string_free(buf, TRUE);
    cli_cfg_anchor_agg_render(agg, output);
    cli_cfg_anchor_agg_free(agg);
    return output;
}

GString *cli_cfg_collect_current_config(uint32_t exclude_module_id)
{
    return cli_cfg_collect_current_config_checked(exclude_module_id, NULL);
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

#define CLI_CFG_DIFF_PARAM_NAME 1
#define CLI_CFG_SNAPSHOT_MAX_BYTES (16U * 1024U * 1024U)

typedef struct cli_cfg_snapshot
{
    gchar *name;
    gchar *path;
    gchar *text;
    gsize text_len;
    cli_config_model_t *model;
} cli_cfg_snapshot_t;

static void cli_cfg_snapshot_clear(cli_cfg_snapshot_t *snapshot)
{
    if (!snapshot)
    {
        return;
    }
    g_free(snapshot->name);
    g_free(snapshot->path);
    g_free(snapshot->text);
    cli_config_model_free(snapshot->model);
    memset(snapshot, 0, sizeof(*snapshot));
}

static gboolean cli_cfg_snapshot_name_parse(const gchar *arg, gchar **name)
{
    if (name)
    {
        *name = NULL;
    }
    if (!arg || !name)
    {
        return FALSE;
    }

    gsize length = strlen(arg);
    if (length > 4 && g_str_has_suffix(arg, ".cfg"))
    {
        length -= 4;
    }
    if (length == 0 || length > 63 || (arg[length] != '\0' && strcmp(arg + length, ".cfg") != 0))
    {
        return FALSE;
    }

    for (gsize i = 0; i < length; i++)
    {
        gchar c = arg[i];
        if (!g_ascii_isalnum(c) && c != '_' && c != '-')
        {
            return FALSE;
        }
    }
    *name = g_strndup(arg, length);
    return TRUE;
}

static gchar *cli_cfg_snapshot_dir(void)
{
    const gchar *work_dir = getenv("NN_WORK_DIR");
    return work_dir && work_dir[0] != '\0' ? g_build_filename(work_dir, "data", "configs", NULL)
                                           : g_build_filename(".", "data", "configs", NULL);
}

static gboolean cli_cfg_snapshot_validate_meta(const cli_cfg_snapshot_t *snapshot, gchar **error_text)
{
    gchar *config_dir = cli_cfg_snapshot_dir();
    gchar *meta_name = g_strdup_printf("%s.meta", snapshot->name);
    gchar *meta_path = g_build_filename(config_dir, meta_name, NULL);
    g_free(meta_name);
    g_free(config_dir);

    gchar *meta = NULL;
    if (!g_file_get_contents(meta_path, &meta, NULL, NULL))
    {
        /* 兼容升级前没有完整性元数据的命名快照。 */
        g_free(meta_path);
        return TRUE;
    }
    g_free(meta_path);

    gboolean capture_declared = FALSE;
    gboolean capture_complete = FALSE;
    gchar expected_sha[65] = "";
    gchar format[64] = "";
    gchar **lines = g_strsplit(meta, "\n", -1);
    for (guint i = 0; lines && lines[i]; i++)
    {
        gchar *line = g_strstrip(lines[i]);
        if (g_str_has_prefix(line, "capture_complete="))
        {
            capture_declared = TRUE;
            capture_complete = strcmp(g_strstrip(line + strlen("capture_complete=")), "1") == 0;
        }
        else if (g_str_has_prefix(line, "cfg_sha256="))
        {
            g_strlcpy(expected_sha, g_strstrip(line + strlen("cfg_sha256=")), sizeof(expected_sha));
        }
        else if (g_str_has_prefix(line, "format="))
        {
            g_strlcpy(format, g_strstrip(line + strlen("format=")), sizeof(format));
        }
    }
    g_strfreev(lines);
    g_free(meta);

    if (capture_declared && !capture_complete)
    {
        if (error_text)
        {
            *error_text = g_strdup("Configuration capture is marked incomplete");
        }
        return FALSE;
    }
    if (format[0] != '\0' && strcmp(format, "bdr-indent-v1") != 0)
    {
        if (error_text)
        {
            *error_text = g_strdup_printf("Unsupported configuration format '%s'", format);
        }
        return FALSE;
    }
    if (expected_sha[0] != '\0')
    {
        gchar *actual_sha =
            g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)snapshot->text, snapshot->text_len);
        gboolean matches =
            actual_sha && strlen(expected_sha) == 64 && g_ascii_strcasecmp(actual_sha, expected_sha) == 0;
        g_free(actual_sha);
        if (!matches)
        {
            if (error_text)
            {
                *error_text = g_strdup("Configuration cfg checksum mismatch");
            }
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean cli_cfg_snapshot_load(const gchar *arg, cli_cfg_snapshot_t *snapshot, gchar **error_text)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (error_text)
    {
        *error_text = NULL;
    }
    if (!cli_cfg_snapshot_name_parse(arg, &snapshot->name))
    {
        if (error_text)
        {
            *error_text = g_strdup("Invalid snapshot name (allowed: A-Z a-z 0-9 _ - and optional .cfg)");
        }
        return FALSE;
    }

    gchar *config_dir = cli_cfg_snapshot_dir();
    gchar *file_name = g_strdup_printf("%s.cfg", snapshot->name);
    snapshot->path = g_build_filename(config_dir, file_name, NULL);
    g_free(file_name);
    g_free(config_dir);

    GError *read_error = NULL;
    if (!g_file_get_contents(snapshot->path, &snapshot->text, &snapshot->text_len, &read_error))
    {
        if (error_text)
        {
            *error_text = g_strdup_printf("Unable to read configuration file '%s': %s", arg,
                                          read_error ? read_error->message : "read failed");
        }
        g_clear_error(&read_error);
        cli_cfg_snapshot_clear(snapshot);
        return FALSE;
    }
    if (snapshot->text_len == 0 || snapshot->text_len > CLI_CFG_SNAPSHOT_MAX_BYTES)
    {
        if (error_text)
        {
            *error_text = g_strdup(snapshot->text_len == 0 ? "Configuration snapshot is empty"
                                                           : "Configuration snapshot exceeds 16 MiB");
        }
        cli_cfg_snapshot_clear(snapshot);
        return FALSE;
    }
    if (!cli_cfg_snapshot_validate_meta(snapshot, error_text))
    {
        cli_cfg_snapshot_clear(snapshot);
        return FALSE;
    }

    GError *parse_error = NULL;
    if (!cli_config_model_parse(snapshot->text, &snapshot->model, &parse_error))
    {
        if (error_text)
        {
            *error_text = g_strdup_printf("Invalid hierarchical configuration: %s",
                                          parse_error ? parse_error->message : "parse failed");
        }
        g_clear_error(&parse_error);
        cli_cfg_snapshot_clear(snapshot);
        return FALSE;
    }
    return TRUE;
}

static gboolean cli_cfg_current_model(cli_config_model_t **model, GString **text, gchar **error_text)
{
    gboolean complete = FALSE;
    GString *current = cli_cfg_collect_current_config_checked(0, &complete);
    if (!complete)
    {
        if (error_text)
        {
            *error_text = g_strdup("Current configuration capture is incomplete; operation refused");
        }
        g_string_free(current, TRUE);
        return FALSE;
    }

    GError *parse_error = NULL;
    cli_config_model_t *parsed = NULL;
    if (!cli_config_model_parse(current->str, &parsed, &parse_error))
    {
        if (error_text)
        {
            *error_text = g_strdup_printf("Current configuration is invalid: %s",
                                          parse_error ? parse_error->message : "parse failed");
        }
        g_clear_error(&parse_error);
        g_string_free(current, TRUE);
        return FALSE;
    }

    *model = parsed;
    if (text)
    {
        *text = current;
    }
    else
    {
        g_string_free(current, TRUE);
    }
    return TRUE;
}

/**
 * @brief show configuration difference current-configuration <snapshot-name> (group_id=12)
 */
static void handle_show_conf_diff(cli_session_t *session, cli_tlv_parser_t *parser)
{
    char *snapshot_name = NULL;

    cli_tlv_entry_t entry;
    int rc = 0;
    while ((rc = cli_tlv_next(parser, &entry)) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == CLI_CFG_DIFF_PARAM_NAME)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text && text[0] != '\0')
            {
                g_free(snapshot_name);
                snapshot_name = g_strdup(text);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (rc < 0)
    {
        cli_send_message(session, "Error: Invalid command parameters.\r\n");
        g_free(snapshot_name);
        return;
    }

    if (!snapshot_name)
    {
        cli_send_message(session, "Error: Missing snapshot name.\r\n");
        return;
    }

    cli_cfg_snapshot_t target;
    gchar *error_text = NULL;
    if (!cli_cfg_snapshot_load(snapshot_name, &target, &error_text))
    {
        gchar *msg = g_strdup_printf("Error: %s.\r\n", error_text ? error_text : "Snapshot load failed");
        cli_send_message(session, msg);
        g_free(msg);
        g_free(error_text);
        g_free(snapshot_name);
        return;
    }

    cli_config_model_t *current_model = NULL;
    GString *current = NULL;
    if (!cli_cfg_current_model(&current_model, &current, &error_text))
    {
        gchar *msg = g_strdup_printf("Error: %s.\r\n", error_text ? error_text : "Current capture failed");
        cli_send_message(session, msg);
        g_free(msg);
        g_free(error_text);
        cli_cfg_snapshot_clear(&target);
        g_free(snapshot_name);
        return;
    }

    GString *diff = cli_config_model_diff(current_model, target.model);
    if (diff->len > 0)
    {
        cli_pager_output(session, diff->str);
    }
    else
    {
        cli_send_message(session, "No configuration difference.\r\n");
    }

    g_string_free(diff, TRUE);
    cli_config_model_free(current_model);
    g_string_free(current, TRUE);
    cli_cfg_snapshot_clear(&target);
    g_free(snapshot_name);
}

#define CLI_CFG_ROLLBACK_ACT_APPLY 1
#define CLI_CFG_ROLLBACK_PARAM_NAME 2

static GMutex g_cli_rollback_mutex;

static gboolean cli_cfg_rollback_parse_request(cli_tlv_parser_t *parser, gchar **name)
{
    *name = NULL;
    gboolean apply = FALSE;

    cli_tlv_entry_t entry;
    int rc = 0;
    while ((rc = cli_tlv_next(parser, &entry)) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CFG_ROLLBACK_ACT_APPLY)
            {
                apply = TRUE;
            }
            else if (entry.cfg_id == CLI_CFG_ROLLBACK_PARAM_NAME)
            {
                const gchar *value = cli_tlv_entry_get_text(&entry);
                if (value && value[0] != '\0')
                {
                    g_free(*name);
                    *name = g_strdup(value);
                }
            }
        }
        cli_tlv_entry_free(&entry);
    }
    return rc >= 0 && apply && *name != NULL;
}

static gboolean cli_cfg_build_rollback_plan(const cli_config_model_t *current, const cli_config_model_t *target,
                                            cli_config_plan_t **plan, gchar **error_text)
{
    cli_view_node_t *config_view = cli_view_find_by_name(g_cli_local->view_tree.root, CLI_VIEW_CONFIG);
    if (!config_view)
    {
        *error_text = g_strdup("CLI config view is unavailable");
        return FALSE;
    }

    GError *plan_error = NULL;
    if (!cli_config_plan_build(current, target, config_view, g_cli_local->view_tree.root,
                               g_cli_local->view_tree.global_cmd_tree, plan, &plan_error))
    {
        *error_text = g_strdup_printf("Rollback preflight failed: %s",
                                      plan_error ? plan_error->message : "plan generation failed");
        g_clear_error(&plan_error);
        return FALSE;
    }
    return TRUE;
}

static gboolean cli_cfg_output_has_error(const gchar *text)
{
    if (!text || text[0] == '\0')
    {
        return FALSE;
    }
    gchar *lower = g_ascii_strdown(text, -1);
    gboolean has_error = strstr(lower, "error:") != NULL;
    g_free(lower);
    return has_error;
}

static gboolean cli_cfg_execute_plan(const cli_config_plan_t *plan, gchar **error_text)
{
    if (error_text)
    {
        *error_text = NULL;
    }

    cli_session_t *internal = g_malloc0(sizeof(*internal));
    internal->internal_session = 1;
    internal->current_view = g_cli_local->view_tree.root;
    internal->out = g_string_new("");
    update_prompt_from_template(internal, internal->current_view->prompt_template);

    if (!process_command("config", internal) || internal->prompt_stack_depth != 1)
    {
        if (error_text)
        {
            *error_text = g_strdup("Failed to enter config view");
        }
        cli_session_destroy(internal);
        return FALSE;
    }

    gboolean success = TRUE;
    for (guint i = 0; plan && plan->steps && i < plan->steps->len; i++)
    {
        const cli_config_plan_step_t *step = g_ptr_array_index(plan->steps, i);
        guint expected_before = step->depth + 1;
        if (internal->prompt_stack_depth != expected_before)
        {
            if (error_text)
            {
                *error_text = g_strdup_printf("Plan step %u `%s` expected view depth %u, actual %u", i + 1,
                                              step->command, expected_before, internal->prompt_stack_depth);
            }
            success = FALSE;
            break;
        }

        gint expected_after = (gint)expected_before + step->view_delta;
        if (expected_after < 1 || expected_after > CLI_PROMPT_STACK_DEPTH)
        {
            if (error_text)
            {
                *error_text = g_strdup_printf("Plan step %u `%s` exceeds supported view depth", i + 1, step->command);
            }
            success = FALSE;
            break;
        }

        g_string_truncate(internal->out, 0);
        int ok = process_command(step->command, internal);
        if (!ok || cli_cfg_output_has_error(internal->out->str) ||
            internal->prompt_stack_depth != (guint)expected_after)
        {
            if (error_text)
            {
                gchar *response = g_strdup(internal->out->str ? internal->out->str : "");
                g_strstrip(response);
                *error_text =
                    g_strdup_printf("Plan step %u `%s` failed%s%s (expected depth %d, actual %u)", i + 1, step->command,
                                    response[0] ? ": " : "", response, expected_after, internal->prompt_stack_depth);
                g_free(response);
            }
            success = FALSE;
            break;
        }
    }

    if (success && internal->prompt_stack_depth != 1)
    {
        if (error_text)
        {
            *error_text =
                g_strdup_printf("Rollback plan ended at view depth %u instead of config", internal->prompt_stack_depth);
        }
        success = FALSE;
    }

    /* 无论成功与否都只重置内部逻辑会话，不把当前视图泄漏到后续操作。 */
    g_string_truncate(internal->out, 0);
    (void)process_command("end", internal);
    cli_session_destroy(internal);
    return success;
}

static gchar *cli_cfg_model_mismatch_summary(const cli_config_model_t *actual, const cli_config_model_t *expected)
{
    GString *difference = cli_config_model_diff(actual, expected);
    if (difference->len > 2048)
    {
        g_string_truncate(difference, 2048);
        g_string_append(difference, "\r\n... (difference truncated)");
    }
    return g_string_free(difference, FALSE);
}

static gboolean cli_cfg_verify_model(const cli_config_model_t *expected, gchar **error_text)
{
    cli_config_model_t *actual = NULL;
    gchar *capture_error = NULL;
    if (!cli_cfg_current_model(&actual, NULL, &capture_error))
    {
        if (error_text)
        {
            *error_text = capture_error;
        }
        else
        {
            g_free(capture_error);
        }
        return FALSE;
    }

    gboolean matches = !cli_config_model_has_diff(actual, expected);
    if (!matches && error_text)
    {
        gchar *summary = cli_cfg_model_mismatch_summary(actual, expected);
        *error_text = g_strdup_printf("Post-apply verification mismatch:\r\n%s", summary);
        g_free(summary);
    }
    cli_config_model_free(actual);
    return matches;
}

static gboolean cli_cfg_compensate_to(const cli_config_model_t *original, gchar **error_text)
{
    cli_config_model_t *actual = NULL;
    gchar *detail = NULL;
    if (!cli_cfg_current_model(&actual, NULL, &detail))
    {
        if (error_text)
        {
            *error_text = detail;
        }
        else
        {
            g_free(detail);
        }
        return FALSE;
    }

    if (!cli_config_model_has_diff(actual, original))
    {
        cli_config_model_free(actual);
        return TRUE;
    }

    cli_config_plan_t *compensation = NULL;
    if (!cli_cfg_build_rollback_plan(actual, original, &compensation, &detail))
    {
        cli_config_model_free(actual);
        if (error_text)
        {
            *error_text = detail;
        }
        else
        {
            g_free(detail);
        }
        return FALSE;
    }
    cli_config_model_free(actual);

    gboolean restored = cli_cfg_execute_plan(compensation, &detail);
    cli_config_plan_free(compensation);
    if (restored)
    {
        restored = cli_cfg_verify_model(original, &detail);
    }

    if (!restored)
    {
        if (error_text)
        {
            *error_text = detail;
        }
        else
        {
            g_free(detail);
        }
        return FALSE;
    }
    g_free(detail);
    return TRUE;
}

static void handle_apply_rollback(cli_session_t *session, const gchar *name)
{
    if (!g_mutex_trylock(&g_cli_rollback_mutex))
    {
        cli_send_message(session, "Error: Another configuration rollback is already running.\r\n");
        return;
    }

    cli_cfg_snapshot_t target;
    memset(&target, 0, sizeof(target));
    cli_config_model_t *original = NULL;
    cli_config_plan_t *forward = NULL;
    cli_config_plan_t *reverse_preflight = NULL;
    gchar *error_text = NULL;
    gchar *result_text = NULL;

    if (!cli_cfg_snapshot_load(name, &target, &error_text))
    {
        result_text = g_strdup_printf("Error: %s.\r\n", error_text ? error_text : "Snapshot load failed");
        goto done;
    }
    if (!cli_cfg_current_model(&original, NULL, &error_text))
    {
        result_text = g_strdup_printf("Error: %s.\r\n", error_text ? error_text : "Current capture failed");
        goto done;
    }
    if (!cli_cfg_build_rollback_plan(original, target.model, &forward, &error_text))
    {
        result_text = g_strdup_printf("Error: %s.\r\n", error_text ? error_text : "Rollback preflight failed");
        goto done;
    }
    if (cli_config_plan_is_empty(forward))
    {
        result_text = g_strdup("No rollback required; running configuration already matches snapshot.\r\n");
        goto done;
    }

    /*
     * 执行前也预检 target -> original。它不能提供跨模块原子事务，但可保证目标状态
     * 至少存在一条可求解的补偿路径；真正失败时仍以实际 running BDR 重算补偿计划。
     */
    if (!cli_cfg_build_rollback_plan(target.model, original, &reverse_preflight, &error_text))
    {
        result_text = g_strdup_printf("Error: Rollback refused because compensation preflight failed: %s.\r\n",
                                      error_text ? error_text : "unknown error");
        goto done;
    }

    /* 乐观并发保护：计划生成期间 running 若已变化，不执行旧计划。 */
    cli_config_model_t *before_apply = NULL;
    if (!cli_cfg_current_model(&before_apply, NULL, &error_text))
    {
        result_text = g_strdup_printf("Error: %s.\r\n", error_text ? error_text : "Current capture failed");
        goto done;
    }
    gboolean changed_during_plan = cli_config_model_has_diff(before_apply, original);
    cli_config_model_free(before_apply);
    if (changed_during_plan)
    {
        result_text = g_strdup("Error: Running configuration changed during rollback planning; retry the command.\r\n");
        goto done;
    }

    gboolean applied = cli_cfg_execute_plan(forward, &error_text);
    if (applied)
    {
        applied = cli_cfg_verify_model(target.model, &error_text);
    }
    if (applied)
    {
        result_text = g_strdup_printf(
            "Configuration rolled back to '%s' successfully (%u command step(s)); post-apply verification passed.\r\n",
            target.name, forward->steps->len);
        goto done;
    }

    gchar *apply_error = g_strdup(error_text ? error_text : "unknown apply failure");
    g_clear_pointer(&error_text, g_free);
    gchar *compensation_error = NULL;
    gboolean compensated = cli_cfg_compensate_to(original, &compensation_error);
    if (compensated)
    {
        result_text = g_strdup_printf(
            "Error: Rollback to '%s' failed: %s. Original running configuration was restored and verified.\r\n",
            target.name, apply_error);
    }
    else
    {
        result_text =
            g_strdup_printf("Error: Rollback to '%s' failed: %s. Compensation also failed: %s. Running configuration "
                            "may be partial; inspect `show current-configuration` immediately.\r\n",
                            target.name, apply_error, compensation_error ? compensation_error : "unknown error");
    }
    g_free(apply_error);
    g_free(compensation_error);

done:
    g_free(error_text);
    cli_config_plan_free(reverse_preflight);
    cli_config_plan_free(forward);
    cli_config_model_free(original);
    cli_cfg_snapshot_clear(&target);
    g_mutex_unlock(&g_cli_rollback_mutex);

    if (result_text)
    {
        cli_send_message(session, result_text);
        g_free(result_text);
    }
}

static void handle_config_rollback(cli_session_t *session, cli_tlv_parser_t *parser)
{
    gchar *name = NULL;
    if (!cli_cfg_rollback_parse_request(parser, &name))
    {
        cli_send_message(session, "Error: Invalid rollback command parameters.\r\n");
        g_free(name);
        return;
    }

    handle_apply_rollback(session, name);
    g_free(name);
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
    gboolean complete = TRUE;
    GString *output = cli_cfg_collect_current_config_checked(0, &complete);
    if (!complete)
    {
        g_string_free(output, TRUE);
        cli_send_message(session,
                         "Error: Current configuration capture is incomplete; no partial output was shown.\r\n");
        return;
    }
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
    gboolean complete = TRUE;

    uint32_t num_mod_ids = 0;
    const uint32_t *mod_ids = cli_view_get_show_this_modules(session->current_view, &num_mod_ids);
    for (uint32_t i = 0; i < num_mod_ids; i++)
    {
        uint32_t mod_id = mod_ids[i];
        if (!is_show_config_module_connected(mod_id))
        {
            continue;
        }

        if (!collect_module_show_config(mod_id, payload, payload_len, buf))
        {
            complete = FALSE;
            break;
        }
        if (buf->len > 0)
        {
            cli_cfg_anchor_agg_feed(agg, buf->str);
        }
    }

    GString *output = g_string_new("");
    if (complete)
    {
        cli_cfg_anchor_agg_render(agg, output);
        render_show_config_output(session, output);
    }
    else
    {
        cli_send_message(session, "Error: Current view configuration capture failed; no partial output was shown.\r\n");
    }
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
        case CLI_GROUP_ID_CONFIG_ROLLBACK:
            handle_config_rollback(session, &parser);
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
