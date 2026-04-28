/**
 * @file   if_cli.c
 * @brief  接口模块 CLI 配置命令处理：IPC 线程解析 TLV，worker 线程同步执行 apply
 * @author jhb
 * @date   2026/01/22
 */
#include "if_cli.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "if_db.h"
#include "if_main.h"
#include "log.h"
#include "net_addr.h"
#include "work/if_worker.h"

/** loop 接口编号范围 */
#define IF_LOOP_ID_MIN 1U
#define IF_LOOP_ID_MAX 1024U

// ============================================================================
// 发送响应辅助
// ============================================================================

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_IF, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(if_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 接口名称映射表（cfg_id → 逻辑接口名）
// ============================================================================

static const char *if_cfgid_to_name(uint32_t cfg_id)
{
    switch (cfg_id)
    {
        case 0:
            return "null0";
        case 1:
            return "GE-1";
        case 2:
            return "GE-2";
        case 3:
            return "GE-3";
        case 4:
            return "GE-4";
        case 5:
            return "GE-5";
        case 6:
            return "GE-6";
        case 7:
            return "GE-7";
        case 8:
            return "GE-8";
        default:
            return NULL;
    }
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理接口进入命令（if GE-x/null0 → 视图切换）
 */
static int handle_if_entry(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理接口配置命令（ip address / ipv6 address / shutdown / no shutdown）
 */
static int handle_if_config(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    net_prefix_t prefix;
    memset(&prefix, 0, sizeof(prefix));
    gboolean has_addr = FALSE;
    gboolean has_shutdown = FALSE;
    gboolean bad_addr_arg = FALSE;
    sa_family_t addr_family = 0;
    uint32_t if_idx = 0;
    uint32_t loop_id = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_IF_IDX)
            {
                if_idx = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            else if (entry.cfg_id == CLI_CTX_ID_IF_LOOP_IDX)
            {
                loop_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text && net_addr_from_str(text, &prefix.addr) == 0 && prefix.addr.family == AF_INET)
                {
                    has_addr = TRUE;
                    addr_family = AF_INET;
                }
                else
                {
                    bad_addr_arg = TRUE;
                }
                break;
            }
            case 2:
            {
                prefix.prefix_len = (uint8_t)cli_tlv_entry_get_int(&entry);
                if (addr_family == 0)
                {
                    addr_family = AF_INET;
                }
                break;
            }
            case 6:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text && net_addr_from_str(text, &prefix.addr) == 0 && prefix.addr.family == AF_INET6)
                {
                    has_addr = TRUE;
                    addr_family = AF_INET6;
                }
                else
                {
                    bad_addr_arg = TRUE;
                }
                break;
            }
            case 7:
            {
                prefix.prefix_len = (uint8_t)cli_tlv_entry_get_int(&entry);
                if (addr_family == 0)
                {
                    addr_family = AF_INET6;
                }
                break;
            }
            case 3:
                has_shutdown = TRUE;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    char loop_name_buf[32];
    const char *ifname = NULL;
    if (loop_id >= IF_LOOP_ID_MIN && loop_id <= IF_LOOP_ID_MAX)
    {
        snprintf(loop_name_buf, sizeof(loop_name_buf), "loop%u", loop_id);
        ifname = loop_name_buf;
    }
    else
    {
        ifname = if_cfgid_to_name(if_idx);
    }

    if (!ifname)
    {
        send_resp(msg, "Error: No interface selected\r\n");
        return ERRCODE_FAIL;
    }

    gboolean is_null0 = (strcmp(ifname, "null0") == 0);

    if (bad_addr_arg)
    {
        send_resp(msg, "Error: Invalid address argument\r\n");
        return ERRCODE_FAIL;
    }

    if (is_null0 && (has_addr || has_shutdown))
    {
        send_resp(msg, "Error: null0 does not support ip address or shutdown\r\n");
        return ERRCODE_FAIL;
    }

    if (loop_id > 0 && has_shutdown)
    {
        send_resp(msg, "Error: loop 接口不支持 shutdown\r\n");
        return ERRCODE_FAIL;
    }

    if (has_addr)
    {
        if_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = IF_APPLY_OP_IP_SET;
        apply.u.ip_set.is_no = is_no ? 1 : 0;
        apply.u.ip_set.prefix = prefix;
        g_strlcpy(apply.u.ip_set.ifname, ifname, sizeof(apply.u.ip_set.ifname));

        if (if_worker_dispatch_apply(&apply) != ERRCODE_SUCCESS || apply.rc != ERRCODE_SUCCESS)
        {
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Error: Failed to set IP address on %s\r\n", ifname);
            send_resp(msg, resp_buf);
            return ERRCODE_FAIL;
        }

        char ip_str[64] = "";
        uint8_t stored_prefix_len = 0;
        if (!is_no)
        {
            if (!net_prefix_is_set(&prefix))
            {
                send_resp(msg, "Error: Failed to read configured address\r\n");
                return ERRCODE_FAIL;
            }
            net_addr_to_str(&prefix.addr, ip_str, sizeof(ip_str));
            stored_prefix_len = prefix.prefix_len;
        }

        if (if_db_update_ip(ifname, (addr_family == AF_INET6), ip_str, is_no ? 0 : stored_prefix_len) !=
            ERRCODE_SUCCESS)
        {
            LOG_WARN("Failed to update db for ip address on %s", ifname);
        }

        char resp_buf[128];
        snprintf(resp_buf, sizeof(resp_buf), "IP address %s on %s\r\n", is_no ? "cleared" : "configured", ifname);
        send_resp(msg, resp_buf);
    }
    else if (has_shutdown)
    {
        if_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = IF_APPLY_OP_SHUTDOWN_SET;
        apply.u.shutdown_set.is_no = is_no ? 1 : 0;
        g_strlcpy(apply.u.shutdown_set.ifname, ifname, sizeof(apply.u.shutdown_set.ifname));

        if (if_worker_dispatch_apply(&apply) != ERRCODE_SUCCESS || apply.rc != ERRCODE_SUCCESS)
        {
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Error: Failed to change state for %s\r\n", ifname);
            send_resp(msg, resp_buf);
            return ERRCODE_FAIL;
        }

        if (if_db_update_shutdown(ifname, is_no ? 0 : 1) != ERRCODE_SUCCESS)
        {
            LOG_WARN("Failed to update db for shutdown on %s", ifname);
        }

        char resp_buf[128];
        snprintf(resp_buf, sizeof(resp_buf), "Interface %s %s\r\n", ifname, is_no ? "enabled" : "disabled");
        send_resp(msg, resp_buf);
    }

    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 loop 接口进入/删除命令（if loop <N> / no if loop <N>）
 */
static int handle_if_loop_entry(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t loop_id = 0;

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
            loop_id = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (loop_id < IF_LOOP_ID_MIN || loop_id > IF_LOOP_ID_MAX)
    {
        send_resp(msg, "Error: loop 接口编号超出范围（1-1024）\r\n");
        return ERRCODE_FAIL;
    }

    char name[32];
    snprintf(name, sizeof(name), "loop%u", loop_id);

    if_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    if (is_no)
    {
        /* 删除：先 worker 撤销内存+OS 接口，再 CLI 线程清 DB（worker 失败时不删 DB，
         * 重启可由 if_db_restore 重建内存态） */
        apply.op = IF_APPLY_OP_LOOP_DELETE;
        apply.u.loop_delete.loop_id = loop_id;
        if (if_worker_dispatch_apply(&apply) != ERRCODE_SUCCESS || apply.rc != ERRCODE_SUCCESS)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "Error: loop%u 不存在或删除失败\r\n", loop_id);
            send_resp(msg, buf);
            return ERRCODE_FAIL;
        }
        (void)if_db_del_record(name);

        char buf[64];
        snprintf(buf, sizeof(buf), "loop%u 已删除\r\n", loop_id);
        send_resp(msg, buf);
    }
    else
    {
        /* 创建：先 CLI 线程写 DB，再派发 worker 创建内存+OS。worker 若失败留下孤儿 DB 行，
         * 下次 if_db_restore 会幂等重建。 */
        if (if_db_ensure_record(name) != ERRCODE_SUCCESS)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "Error: 写入 loop%u DB 失败\r\n", loop_id);
            send_resp(msg, buf);
            return ERRCODE_FAIL;
        }

        apply.op = IF_APPLY_OP_LOOP_CREATE;
        apply.u.loop_create.loop_id = loop_id;
        if (if_worker_dispatch_apply(&apply) != ERRCODE_SUCCESS || apply.rc != ERRCODE_SUCCESS)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "Error: 创建 loop%u 失败\r\n", loop_id);
            send_resp(msg, buf);
            return ERRCODE_FAIL;
        }
        send_resp(msg, "");
    }

    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int if_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("Payload parsing failed");
        send_resp(msg, "IF Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("Received TLV payload (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case IF_CLI_GROUP_ID_ENTRY:
            result = handle_if_entry(msg, &parser);
            break;
        case IF_CLI_GROUP_ID_CONFIG:
            result = handle_if_config(msg, &parser);
            break;
        case IF_CLI_GROUP_ID_LOOP_ENTRY:
            result = handle_if_loop_entry(msg, &parser);
            break;
        default:
            LOG_WARN("Unknown group_id: %u", parser.group_id);
            send_resp(msg, "IF Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
