/**
 * @file   dev_cli.c
 * @brief  Dev 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#define LOG_TAG "dev"

#include "dev_cli.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "dev_main.h"
#include "dev_module.h"
#include "errcode.h"
#include "log.h"
#include "path_utils.h"

// ============================================================================
// 内部辅助函数：show 命令
// ============================================================================

typedef struct show_module_ctx
{
    dev_cli_resp_out_t *resp;
    ipc_context_t      *ipc_ctx;
} show_module_ctx_t;

static const char *log_level_to_string(log_level_t level)
{
    switch (level)
    {
        case LOG_LEVEL_DEBUG:
            return "debug";
        case LOG_LEVEL_INFO:
            return "info";
        case LOG_LEVEL_WARN:
            return "warn";
        case LOG_LEVEL_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static const char *build_profile_string(void)
{
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

static const char *asan_enabled_string(void)
{
#if defined(__has_feature)
#    if __has_feature(address_sanitizer)
    return "yes";
#    else
    return "no";
#    endif
#elif defined(__SANITIZE_ADDRESS__)
    return "yes";
#else
    return "no";
#endif
}

static int file_read_first_line(const char *path, char *out, size_t out_size)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        return ERRCODE_FAIL;
    }

    if (!fgets(out, (int)out_size, fp))
    {
        fclose(fp);
        return ERRCODE_FAIL;
    }
    fclose(fp);

    size_t n = strcspn(out, "\r\n");
    out[n] = '\0';
    return (out[0] != '\0') ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int resolve_version_file(char *path, size_t path_size)
{
    const char *env_version_file = getenv("NETNEXUS_VERSION_FILE");
    if (env_version_file && env_version_file[0] != '\0')
    {
        snprintf(path, path_size, "%s", env_version_file);
        return ERRCODE_SUCCESS;
    }

    const char *resources_dir = getenv("RESOURCES_DIR");
    if (resources_dir && resources_dir[0] != '\0')
    {
        snprintf(path, path_size, "%s/../VERSION", resources_dir);
        if (access(path, R_OK) == 0)
        {
            return ERRCODE_SUCCESS;
        }
        snprintf(path, path_size, "%s/VERSION", resources_dir);
        if (access(path, R_OK) == 0)
        {
            return ERRCODE_SUCCESS;
        }
    }

    snprintf(path, path_size, "/opt/netnexus/VERSION");
    if (access(path, R_OK) == 0)
    {
        return ERRCODE_SUCCESS;
    }

    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        snprintf(path, path_size, "%s/../VERSION", exe_dir);
        if (access(path, R_OK) == 0)
        {
            return ERRCODE_SUCCESS;
        }

        snprintf(path, path_size, "%s/../../VERSION", exe_dir);
        if (access(path, R_OK) == 0)
        {
            return ERRCODE_SUCCESS;
        }
    }

    snprintf(path, path_size, "VERSION");
    if (access(path, R_OK) == 0)
    {
        return ERRCODE_SUCCESS;
    }

    return ERRCODE_FAIL;
}

static const char *dev_phase_to_string(uint8_t phase)
{
    switch (phase)
    {
        case DEV_PHASE_REGISTERED:
            return "REGISTERED";
        case DEV_PHASE_LOADED:
            return "LOADED";
        case DEV_PHASE_IPC_READY:
            return "IPC_READY";
        case DEV_PHASE_DB_RECOVERED:
            return "DB_RECOVERED";
        case DEV_PHASE_READY:
            return "READY";
        default:
            return "UNKNOWN";
    }
}

static gboolean show_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    show_module_ctx_t *ctx = (show_module_ctx_t *)data;
    dev_cli_resp_out_t *resp = ctx->resp;
    dev_module_t *module = (dev_module_t *)value;
    const char *phase = dev_phase_to_string(module->phase);
    const char *db_name = module->db_name ? module->db_name : "-";
    const char *ipc_state =
        (module->module_id == DEV_MODULE_ID_DEV || ipc_is_connected(ctx->ipc_ctx, module->module_id)) ? "up" : "down";

    char line[192];
    snprintf(line, sizeof(line), "  %-10u %-14s %-12s %-6u %-6s %s\r\n", module->module_id, module->name, phase,
             module->port, ipc_state, db_name);

    strncat(resp->message, line, sizeof(resp->message) - strlen(resp->message) - 1);

    return FALSE;
}

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void dev_send_cli_response(ipc_context_t *ctx, ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    ipc_message_t *resp_msg = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_DEV, msg->src_module_id,
                                                 msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp_msg)
    {
        ipc_send_response(ctx, resp_msg);
        ipc_message_free(resp_msg);
    }
}

// ============================================================================
// 命令处理函数（按 group_id 分发）
// ============================================================================

static int handle_show_module(ipc_context_t *ctx, ipc_message_t *msg)
{
    dev_cli_resp_out_t resp_out;
    show_module_ctx_t show_ctx;
    memset(&resp_out, 0, sizeof(resp_out));
    memset(&show_ctx, 0, sizeof(show_ctx));

    show_ctx.resp = &resp_out;
    show_ctx.ipc_ctx = ctx;

    snprintf(resp_out.message, sizeof(resp_out.message),
             "\r\nRegistered Modules:\r\n"
             "  %-10s %-14s %-12s %-6s %-6s %s\r\n"
             "  ----------------------------------------------------------------\r\n",
             "ID", "Name", "Phase", "Port", "IPC", "DB");

    dev_module_foreach(show_module_callback, &show_ctx);

    strncat(resp_out.message, "\r\n", sizeof(resp_out.message) - strlen(resp_out.message) - 1);

    dev_send_cli_response(ctx, msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

static int handle_show_version(ipc_context_t *ctx, ipc_message_t *msg)
{
    char buf[CLI_MAX_RESP_LEN];
    char version[64] = "unknown";
    char module_name[DEV_MODULE_NAME_MAX_LEN] = "dev";
    char version_path[PATH_MAX];
    size_t off = 0;
    (void)ctx;

#define APPEND_FMT(fmt, ...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        if (off < sizeof(buf))                                                                                         \
        {                                                                                                              \
            int written = snprintf(buf + off, sizeof(buf) - off, fmt, ##__VA_ARGS__);                                 \
            if (written > 0)                                                                                           \
            {                                                                                                          \
                off += (size_t)written;                                                                                \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

    if (resolve_version_file(version_path, sizeof(version_path)) == ERRCODE_SUCCESS)
    {
        if (file_read_first_line(version_path, version, sizeof(version)) != ERRCODE_SUCCESS)
        {
            strlcpy(version, "unknown", sizeof(version));
        }
    }

    if (dev_get_module_name(ctx, DEV_MODULE_ID_DEV, module_name) != ERRCODE_SUCCESS)
    {
        strlcpy(module_name, "dev", sizeof(module_name));
    }

    APPEND_FMT("\r\nNetNexus Version Information:\r\n");
    APPEND_FMT("  Version      : %s\r\n", version);
    APPEND_FMT("  Build Time   : %s %s\r\n", __DATE__, __TIME__);
    APPEND_FMT("  Build Profile: %s\r\n", build_profile_string());
    APPEND_FMT("  Compiler     : %s\r\n", __VERSION__);
    APPEND_FMT("  ASAN         : %s\r\n", asan_enabled_string());
    APPEND_FMT("  Log Level    : %s\r\n", log_level_to_string(log_get_level()));
    APPEND_FMT("  Module       : %s (id=%u)\r\n", module_name, DEV_MODULE_ID_DEV);
    APPEND_FMT("  PID          : %d\r\n\r\n", (int)getpid());

#undef APPEND_FMT

    dev_send_cli_response(ctx, msg, buf);
    return ERRCODE_SUCCESS;
}

static int handle_sysname(ipc_context_t *ctx, ipc_message_t *msg)
{
    dev_send_cli_response(ctx, msg, "Command 'sysname' not yet implemented in dev module.\r\n");
    return ERRCODE_SUCCESS;
}

static int handle_set_log_level(ipc_context_t *ctx, ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    cli_tlv_entry_t entry;

    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 1)
        {
            const char *level_text = cli_tlv_entry_get_text(&entry);
            if (!level_text)
            {
                cli_tlv_entry_free(&entry);
                dev_send_cli_response(ctx, msg, "Dev Error: missing log level parameter.\r\n");
                return ERRCODE_FAIL;
            }

            char level_buf[16];
            strlcpy(level_buf, level_text, sizeof(level_buf));
            cli_tlv_entry_free(&entry);

            if (log_set_level_by_name(level_buf) == 0)
            {
                char resp[128];
                snprintf(resp, sizeof(resp), "Dev: log level set to '%s'.\r\n", log_level_to_string(log_get_level()));
                dev_send_cli_response(ctx, msg, resp);
                return ERRCODE_SUCCESS;
            }

            dev_send_cli_response(ctx, msg, "Dev Error: invalid log level. Use debug/info/warn/error.\r\n");
            return ERRCODE_FAIL;
        }

        cli_tlv_entry_free(&entry);
    }

    dev_send_cli_response(ctx, msg, "Dev Error: missing log level parameter.\r\n");
    return ERRCODE_FAIL;
}

// ============================================================================
// 主入口
// ============================================================================

int dev_cli_handle_continue(ipc_context_t *ctx, ipc_message_t *msg)
{
    dev_send_cli_response(ctx, msg, "");
    return ERRCODE_SUCCESS;
}

int dev_cli_handle_message(ipc_context_t *ctx, ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        dev_send_cli_response(ctx, msg, "Dev Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case DEV_CLI_GROUP_ID_SHOW_VERSION:
            result = handle_show_version(ctx, msg);
            break;
        case DEV_CLI_GROUP_ID_SYSNAME:
            result = handle_sysname(ctx, msg);
            break;
        case DEV_CLI_GROUP_ID_SHOW_MODULE:
            result = handle_show_module(ctx, msg);
            break;
        case DEV_CLI_GROUP_ID_LOG_LEVEL:
            result = handle_set_log_level(ctx, msg, &parser);
            break;
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            dev_send_cli_response(ctx, msg, "Dev Error: Unknown command.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
