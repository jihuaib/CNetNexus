/**
 * @file   dev_cli.c
 * @brief  Dev 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */

#include "dev_cli.h"

#include <arpa/inet.h>
#include <glib.h>
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

static gint g_reboot_in_progress = 0;

// ============================================================================
// 内部辅助函数：show 命令
// ============================================================================

typedef struct show_module_ctx
{
    GString *resp;
    dev_ipc_context_t *dev_ipc_ctx;
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
    char *p = NULL;

    /* 生产环境 1: 环境变量 NN_WORK_DIR */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir && work_dir[0] != '\0')
    {
        p = g_build_filename(work_dir, "VERSION", NULL);
        if (access(p, R_OK) == 0)
        {
            strlcpy(path, p, path_size);
            g_free(p);
            return ERRCODE_SUCCESS;
        }
        g_free(p);
    }

    /* 开发环境: 相对于可执行文件的路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        p = g_build_filename(exe_dir, "..", "..", "VERSION", NULL);
        if (access(p, R_OK) == 0)
        {
            strlcpy(path, p, path_size);
            g_free(p);
            return ERRCODE_SUCCESS;
        }
        g_free(p);
    }

    /* 回退: 当前目录 */
    strlcpy(path, "VERSION", path_size);
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
            return "DEV_IPC_READY";
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
    dev_module_t *module = (dev_module_t *)value;
    const char *phase = dev_phase_to_string(module->phase);
    const char *dev_ipc_state =
        (module->module_id == DEV_MODULE_ID_DEV || dev_ipc_is_connected(ctx->dev_ipc_ctx, module->module_id)) ? "up"
                                                                                                              : "down";
    g_string_append_printf(ctx->resp, "  %-10u %-14s %-12s %-6u %s\r\n", module->module_id, module->name, phase,
                           module->port, dev_ipc_state);

    return FALSE;
}

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void dev_send_cli_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp_msg = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_DEV, msg->src_module_id,
                                                         msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp_msg)
    {
        dev_ipc_send_response(ctx, resp_msg);
        dev_ipc_message_free(resp_msg);
    }
}

// ============================================================================
// 命令处理函数（按 group_id 分发）
// ============================================================================

static int handle_show_module(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    show_module_ctx_t show_ctx;
    memset(&show_ctx, 0, sizeof(show_ctx));

    show_ctx.resp = g_string_new("");
    if (!show_ctx.resp)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: out of memory.\r\n");
        return ERRCODE_FAIL;
    }
    show_ctx.dev_ipc_ctx = ctx;

    g_string_append_printf(show_ctx.resp,
                           "\r\nRegistered Modules:\r\n"
                           "  %-10s %-14s %-12s %-6s %s\r\n"
                           "  --------------------------------------------------------\r\n",
                           "ID", "Name", "Phase", "Port", "IPC");

    dev_module_foreach(show_module_callback, &show_ctx);
    g_string_append(show_ctx.resp, "\r\n");
    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, show_ctx.resp);
}

static int handle_show_version(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    char version[64] = "unknown";
    char version_path[PATH_MAX];
    GString *buf = g_string_new("");
    if (!buf)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    if (resolve_version_file(version_path, sizeof(version_path)) == ERRCODE_SUCCESS)
    {
        if (file_read_first_line(version_path, version, sizeof(version)) != ERRCODE_SUCCESS)
        {
            strlcpy(version, "unknown", sizeof(version));
        }
    }

    g_string_append(buf, "\r\nNetNexus Version Information:\r\n");
    g_string_append_printf(buf, "  Version      : %s\r\n", version);
    g_string_append_printf(buf, "  Build Time   : %s %s\r\n", __DATE__, __TIME__);
    g_string_append_printf(buf, "  Build Profile: %s\r\n", build_profile_string());
    g_string_append_printf(buf, "  Compiler     : %s\r\n", __VERSION__);
    g_string_append_printf(buf, "  ASAN         : %s\r\n", asan_enabled_string());
    g_string_append_printf(buf, "  Log Level    : %s\r\n", log_level_to_string(log_get_level()));
    g_string_append_printf(buf, "  PID          : %d\r\n\r\n", (int)getpid());

    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, buf);
}

static int handle_sysname(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    dev_send_cli_response(ctx, msg, "Command 'sysname' not yet implemented in dev module.\r\n");
    return ERRCODE_SUCCESS;
}

static int handle_reboot(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!g_atomic_int_compare_and_exchange(&g_reboot_in_progress, 0, 1))
    {
        dev_send_cli_response(ctx, msg, "Dev: reboot already in progress.\r\n");
        return ERRCODE_FAIL;
    }

    dev_send_cli_response(ctx, msg, "Dev: reboot accepted, reconnect later.\r\n");
    /* 短暂让出，提升 ACK 送达 CLI 客户端的概率。 */
    g_usleep(100 * 1000);

    int ret = dev_reboot_software();
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Software reboot failed");
    }

    g_atomic_int_set(&g_reboot_in_progress, 0);
    return ret;
}

static int handle_set_log_level(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    cli_tlv_entry_t entry;

    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
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

static const char *ipc_state_to_string(dev_ipc_costate_t state)
{
    switch (state)
    {
        case DEV_IPC_CODISCONNECTED:
            return "DISCONNECTED";
        case DEV_IPC_COCONNECTING:
            return "CONNECTING";
        case DEV_IPC_COHANDSHAKING:
            return "HANDSHAKING";
        case DEV_IPC_COCONNECTED:
            return "CONNECTED";
        case DEV_IPC_CORECONNECTING:
            return "RECONNECTING";
        default:
            return "UNKNOWN";
    }
}

/* IPC 连接 wire format 中每条 entry 的大小（见 ipc_context.c 注释） */
#define IPC_QCONNS_ENTRY_SIZE 100

/**
 * @brief 从 QUERY_IPC_CONNS 响应 payload 中解析并格式化第 i 条连接信息到 buf
 */
static void format_ipc_conn_entry(const uint8_t *entry, int idx, GString *buf)
{
    const uint8_t *p = entry;
    uint32_t v;
    uint16_t v16;

    memcpy(&v, p, 4);
    uint32_t remote_module_id = ntohl(v);
    p += 4;
    memcpy(&v, p, 4);
    uint32_t state = ntohl(v);
    p += 4;
    memcpy(&v, p, 4);
    uint32_t is_initiator = ntohl(v);
    p += 4;
    char remote_host[65];
    memcpy(remote_host, p, 64);
    remote_host[64] = '\0';
    p += 64;
    memcpy(&v16, p, 2);
    uint16_t remote_port = ntohs(v16);
    p += 4; /* port + pad */

    uint32_t hi, lo;
    memcpy(&hi, p, 4);
    hi = ntohl(hi);
    p += 4;
    memcpy(&lo, p, 4);
    lo = ntohl(lo);
    p += 4;
    time_t last_hb_sent = (time_t)(((uint64_t)hi << 32) | lo);

    memcpy(&hi, p, 4);
    hi = ntohl(hi);
    p += 4;
    memcpy(&lo, p, 4);
    lo = ntohl(lo);
    p += 4;
    time_t last_hb_recv = (time_t)(((uint64_t)hi << 32) | lo);

    memcpy(&v, p, 4);
    uint32_t reconnect_delay_ms = ntohl(v);

    /* 格式化心跳时间 */
    char hb_sent_str[32] = "N/A";
    char hb_recv_str[32] = "N/A";
    if (last_hb_sent > 0)
    {
        struct tm *t = localtime(&last_hb_sent);
        strftime(hb_sent_str, sizeof(hb_sent_str), "%H:%M:%S", t);
    }
    if (last_hb_recv > 0)
    {
        struct tm *t = localtime(&last_hb_recv);
        strftime(hb_recv_str, sizeof(hb_recv_str), "%H:%M:%S", t);
    }

    g_string_append_printf(buf, "  Connection #%d (peer: 0x%08X):\r\n", idx, remote_module_id);
    g_string_append_printf(buf, "    %-16s: %s\r\n", "Direction",
                           is_initiator ? "Active (Initiator)" : "Passive (Acceptor)");
    g_string_append_printf(buf, "    %-16s: %s\r\n", "State", ipc_state_to_string((dev_ipc_costate_t)state));
    if (is_initiator && remote_host[0] != '\0')
    {
        g_string_append_printf(buf, "    %-16s: %s:%u\r\n", "Remote Addr", remote_host, remote_port);
    }
    g_string_append_printf(buf, "    %-16s: %s\r\n", "HB Sent", hb_sent_str);
    g_string_append_printf(buf, "    %-16s: %s\r\n", "HB Recv", hb_recv_str);
    g_string_append_printf(buf, "    %-16s: %u ms\r\n", "Reconnect Delay", reconnect_delay_ms);
}

/**
 * @brief 将 QUERY_IPC_CONNS 响应 payload 格式化为可读文本，追加到 buf[*off]
 */
static void format_conns_payload(const uint8_t *payload, uint32_t payload_len, const char *module_name,
                                 uint32_t target_id, GString *buf)
{
    const uint8_t *p = payload;
    uint32_t v;
    memcpy(&v, p, 4);
    uint32_t num_conns = ntohl(v);
    p += 4;

    g_string_append_printf(buf, "\r\nIPC Connections of module '%s' (ID: 0x%08X) — %u connection(s):\r\n", module_name,
                           target_id, num_conns);

    uint32_t expected_len = 4 + num_conns * IPC_QCONNS_ENTRY_SIZE;
    if (payload_len < expected_len)
    {
        g_string_append_printf(buf, "  Error: response payload truncated (%u < %u bytes).\r\n\r\n", payload_len,
                               expected_len);
    }
    else if (num_conns == 0)
    {
        g_string_append(buf, "  (no connections)\r\n\r\n");
    }
    else
    {
        for (uint32_t i = 0; i < num_conns; i++)
        {
            format_ipc_conn_entry(p + i * IPC_QCONNS_ENTRY_SIZE, (int)i, buf);
            g_string_append(buf, "\r\n");
        }
    }
}

static int handle_show_ipc(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char module_name[DEV_MODULE_NAME_MAX_LEN] = {0};

    /* 解析模块名称参数（cfg_id=1） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strlcpy(module_name, text, sizeof(module_name));
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (module_name[0] == '\0')
    {
        dev_send_cli_response(ctx, msg, "Error: missing module name.\r\n");
        return ERRCODE_FAIL;
    }

    /* 从注册表查找模块 ID */
    uint32_t target_id = 0;
    if (dev_get_module_id_by_name(module_name, &target_id) != ERRCODE_SUCCESS)
    {
        char err[128];
        snprintf(err, sizeof(err), "Error: module '%s' not found in registry.\r\n", module_name);
        dev_send_cli_response(ctx, msg, err);
        return ERRCODE_FAIL;
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    if (target_id == DEV_MODULE_ID_DEV)
    {
        /* 自查询：DEV 无自连接，直接从 ctx 构造载荷 */
        uint32_t pl_len;
        uint8_t *pl = dev_ipc_build_conns_payload(ctx, &pl_len);
        format_conns_payload(pl, pl_len, module_name, target_id, buf);
        g_free(pl);
    }
    else
    {
        /* 向目标模块发 RPC，查询其自身所有 IPC 连接 */
        dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_QUERY_IPC_CONNS, DEV_MODULE_ID_DEV,
                                                        target_id, 0, NULL, 0, NULL);
        if (!req)
        {
            dev_send_cli_response(ctx, msg, "Error: failed to create IPC query message.\r\n");
            return ERRCODE_FAIL;
        }

        dev_ipc_message_t *resp = dev_ipc_query(ctx, target_id, req, 3000);
        dev_ipc_message_free(req);

        if (!resp || !resp->payload || resp->payload_len < 4)
        {
            g_string_append_printf(buf, "\r\nError: no response from module '%s' (timeout or not connected).\r\n\r\n",
                                   module_name);
            dev_send_cli_response(ctx, msg, buf->str);
            g_string_free(buf, TRUE);
            if (resp)
            {
                dev_ipc_message_free(resp);
            }
            return ERRCODE_FAIL;
        }

        format_conns_payload((const uint8_t *)resp->payload, resp->payload_len, module_name, target_id, buf);
        dev_ipc_message_free(resp);
    }

    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, buf);
}

static int handle_ping(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char ip[64] = {0};

    /* 解析目标 IP 地址参数（cfg_id=1） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 1)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strlcpy(ip, text, sizeof(ip));
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (ip[0] == '\0')
    {
        dev_send_cli_response(ctx, msg, "Error: missing IP address\r\n");
        return ERRCODE_FAIL;
    }

    /* 验证 IP 地址格式，防止命令注入 */
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1)
    {
        dev_send_cli_response(ctx, msg, "Error: invalid IP address format\r\n");
        return ERRCODE_FAIL;
    }

    /* 构造 ping 命令并执行 */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ping -c 4 -W 2 %s 2>&1", ip);

    FILE *fp = popen(cmd, "r");
    if (!fp)
    {
        dev_send_cli_response(ctx, msg, "Error: failed to execute ping command\r\n");
        return ERRCODE_FAIL;
    }

    char result[CLI_MAX_RESP_LEN];
    size_t off = 0;
    result[0] = '\0';

    /* 首行空行 */
    off += (size_t)snprintf(result + off, sizeof(result) - off, "\r\n");

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL && off < sizeof(result) - 4)
    {
        /* 去掉行尾 \n，统一替换为 \r\n */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
        {
            line[--len] = '\0';
        }
        off += (size_t)snprintf(result + off, sizeof(result) - off, "%s\r\n", line);
    }
    pclose(fp);

    dev_send_cli_response(ctx, msg, result);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 动态候选值查询（Tab/? 补全）
// ============================================================================

static gboolean collect_module_name_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    GByteArray *buf = (GByteArray *)data;
    g_byte_array_append(buf, (const guint8 *)module->name, (guint)strlen(module->name) + 1);
    return FALSE;
}

void dev_cli_handle_query_candidates(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    GByteArray *buf = g_byte_array_new();
    dev_module_foreach(collect_module_name_cb, buf);

    /* 末尾额外 '\0' 标记结束 */
    guint8 nul = '\0';
    g_byte_array_append(buf, &nul, 1);

    guint payload_len = buf->len;
    uint8_t *payload = g_byte_array_free(buf, FALSE);

    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_QUERY_CANDIDATES_RESP, DEV_MODULE_ID_DEV,
                                                     msg->src_module_id, msg->request_id, payload, payload_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(payload);
    }
    dev_ipc_message_free(msg);
}

// ============================================================================
// 主入口
// ============================================================================

int dev_cli_handle_show_config(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, NULL);
}

int dev_cli_handle_continue(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg);
}

void dev_cli_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_dev_local->show_stream);
}

int dev_cli_handle_message(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_chunk_stream_reset(&g_dev_local->show_stream);

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("Payload parsing failed");
        dev_send_cli_response(ctx, msg, "Dev Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("Received TLV payload (group_id=%u)", parser.group_id);

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
        case DEV_CLI_GROUP_ID_PING:
            result = handle_ping(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_SHOW_IPC:
            result = handle_show_ipc(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_REBOOT:
            result = handle_reboot(ctx, msg);
            break;
        default:
            LOG_WARN("Unknown group_id: %u", parser.group_id);
            dev_send_cli_response(ctx, msg, "Dev Error: Unknown command.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
