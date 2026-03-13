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

// ============================================================================
// 内部辅助函数：show 命令
// ============================================================================

typedef struct show_module_ctx
{
    dev_cli_resp_out_t *resp;
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
    const char *env_version_file = getenv("NETNEXUS_VERSION_FILE");
    if (env_version_file && env_version_file[0] != '\0')
    {
        strlcpy(path, env_version_file, path_size);
        return ERRCODE_SUCCESS;
    }

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
    dev_cli_resp_out_t *resp = ctx->resp;
    dev_module_t *module = (dev_module_t *)value;
    const char *phase = dev_phase_to_string(module->phase);
    const char *dev_ipc_state =
        (module->module_id == DEV_MODULE_ID_DEV || dev_ipc_is_connected(ctx->dev_ipc_ctx, module->module_id)) ? "up"
                                                                                                              : "down";

    char line[192];
    snprintf(line, sizeof(line), "  %-10u %-14s %-12s %-6u %s\r\n", module->module_id, module->name, phase,
             module->port, dev_ipc_state);

    strncat(resp->message, line, sizeof(resp->message) - strlen(resp->message) - 1);

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
    dev_cli_resp_out_t resp_out;
    show_module_ctx_t show_ctx;
    memset(&resp_out, 0, sizeof(resp_out));
    memset(&show_ctx, 0, sizeof(show_ctx));

    show_ctx.resp = &resp_out;
    show_ctx.dev_ipc_ctx = ctx;

    snprintf(resp_out.message, sizeof(resp_out.message),
             "\r\nRegistered Modules:\r\n"
             "  %-10s %-14s %-12s %-6s %s\r\n"
             "  --------------------------------------------------------\r\n",
             "ID", "Name", "Phase", "Port", "IPC");

    dev_module_foreach(show_module_callback, &show_ctx);

    strncat(resp_out.message, "\r\n", sizeof(resp_out.message) - strlen(resp_out.message) - 1);

    dev_send_cli_response(ctx, msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

static int handle_show_version(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    char buf[CLI_MAX_RESP_LEN];
    char version[64] = "unknown";
    char version_path[PATH_MAX];
    size_t off = 0;
    (void)ctx;

    if (resolve_version_file(version_path, sizeof(version_path)) == ERRCODE_SUCCESS)
    {
        if (file_read_first_line(version_path, version, sizeof(version)) != ERRCODE_SUCCESS)
        {
            strlcpy(version, "unknown", sizeof(version));
        }
    }

    CLI_BUF_APPEND(buf, sizeof(buf), off, "\r\nNetNexus Version Information:\r\n");
    CLI_BUF_APPEND(buf, sizeof(buf), off, "  Version      : %s\r\n", version);
    CLI_BUF_APPEND(buf, sizeof(buf), off, "  Build Time   : %s %s\r\n", __DATE__, __TIME__);
    CLI_BUF_APPEND(buf, sizeof(buf), off, "  Build Profile: %s\r\n", build_profile_string());
    CLI_BUF_APPEND(buf, sizeof(buf), off, "  Compiler     : %s\r\n", __VERSION__);
    CLI_BUF_APPEND(buf, sizeof(buf), off, "  ASAN         : %s\r\n", asan_enabled_string());
    CLI_BUF_APPEND(buf, sizeof(buf), off, "  Log Level    : %s\r\n", log_level_to_string(log_get_level()));
    CLI_BUF_APPEND(buf, sizeof(buf), off, "  PID          : %d\r\n\r\n", (int)getpid());

    dev_send_cli_response(ctx, msg, buf);
    return ERRCODE_SUCCESS;
}

static int handle_sysname(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    dev_send_cli_response(ctx, msg, "Command 'sysname' not yet implemented in dev module.\r\n");
    return ERRCODE_SUCCESS;
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
static void format_ipc_conn_entry(const uint8_t *entry, int idx, char *buf, size_t buf_size, size_t *off)
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

    *off +=
        (size_t)snprintf(buf + *off, buf_size - *off, "  Connection #%d (peer: 0x%08X):\r\n", idx, remote_module_id);
    *off += (size_t)snprintf(buf + *off, buf_size - *off, "    %-16s: %s\r\n", "Direction",
                             is_initiator ? "Active (Initiator)" : "Passive (Acceptor)");
    *off += (size_t)snprintf(buf + *off, buf_size - *off, "    %-16s: %s\r\n", "State",
                             ipc_state_to_string((dev_ipc_costate_t)state));
    if (is_initiator && remote_host[0] != '\0')
    {
        *off += (size_t)snprintf(buf + *off, buf_size - *off, "    %-16s: %s:%u\r\n", "Remote Addr", remote_host,
                                 remote_port);
    }
    *off += (size_t)snprintf(buf + *off, buf_size - *off, "    %-16s: %s\r\n", "HB Sent", hb_sent_str);
    *off += (size_t)snprintf(buf + *off, buf_size - *off, "    %-16s: %s\r\n", "HB Recv", hb_recv_str);
    *off +=
        (size_t)snprintf(buf + *off, buf_size - *off, "    %-16s: %u ms\r\n", "Reconnect Delay", reconnect_delay_ms);
}

/**
 * @brief 将 QUERY_IPC_CONNS 响应 payload 格式化为可读文本，追加到 buf[*off]
 */
static void format_conns_payload(const uint8_t *payload, uint32_t payload_len, const char *module_name,
                                 uint32_t target_id, char *buf, size_t buf_size, size_t *off)
{
    const uint8_t *p = payload;
    uint32_t v;
    memcpy(&v, p, 4);
    uint32_t num_conns = ntohl(v);
    p += 4;

    *off += (size_t)snprintf(buf + *off, buf_size - *off,
                             "\r\nIPC Connections of module '%s' (ID: 0x%08X) — %u connection(s):\r\n", module_name,
                             target_id, num_conns);

    uint32_t expected_len = 4 + num_conns * IPC_QCONNS_ENTRY_SIZE;
    if (payload_len < expected_len)
    {
        *off +=
            (size_t)snprintf(buf + *off, buf_size - *off,
                             "  Error: response payload truncated (%u < %u bytes).\r\n\r\n", payload_len, expected_len);
    }
    else if (num_conns == 0)
    {
        *off += (size_t)snprintf(buf + *off, buf_size - *off, "  (no connections)\r\n\r\n");
    }
    else
    {
        for (uint32_t i = 0; i < num_conns && *off < buf_size - 256; i++)
        {
            format_ipc_conn_entry(p + i * IPC_QCONNS_ENTRY_SIZE, (int)i, buf, buf_size, off);
            *off += (size_t)snprintf(buf + *off, buf_size - *off, "\r\n");
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

    char buf[CLI_MAX_RESP_LEN];
    size_t off = 0;

    if (target_id == DEV_MODULE_ID_DEV)
    {
        /* 自查询：DEV 无自连接，直接从 ctx 构造载荷 */
        uint32_t pl_len;
        uint8_t *pl = dev_ipc_build_conns_payload(ctx, &pl_len);
        format_conns_payload(pl, pl_len, module_name, target_id, buf, sizeof(buf), &off);
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
            off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                    "\r\nError: no response from module '%s' (timeout or not connected).\r\n\r\n",
                                    module_name);
            dev_send_cli_response(ctx, msg, buf);
            if (resp)
            {
                dev_ipc_message_free(resp);
            }
            return ERRCODE_FAIL;
        }

        format_conns_payload((const uint8_t *)resp->payload, resp->payload_len, module_name, target_id, buf,
                             sizeof(buf), &off);
        dev_ipc_message_free(resp);
    }

    dev_send_cli_response(ctx, msg, buf);
    return ERRCODE_SUCCESS;
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

int dev_cli_handle_continue(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    dev_send_cli_response(ctx, msg, "");
    return ERRCODE_SUCCESS;
}

int dev_cli_handle_message(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
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
        case DEV_CLI_GROUP_ID_PING:
            result = handle_ping(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_SHOW_IPC:
            result = handle_show_ipc(ctx, msg, &parser);
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
