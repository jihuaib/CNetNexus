/**
 * @file   bgp_bmp_cli.c
 * @brief  BGP BMP 上报功能 CLI 命令处理
 * @author jhb
 * @date   2026/03/29
 */
#include "bgp_bmp_cli.h"

#include <arpa/inet.h>
#include <string.h>

#include "bgp_bmp_db.h"
#include "bgp_cli.h"
#include "bgp_worker.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// BMP 上下文解析
// ============================================================================

/** BMP CLI 上下文（从 TLV 中提取实例名） */
typedef struct bgp_bmp_cli_ctx
{
    char inst_name[32]; /**< BMP 实例名（从 ctx-id=7 提取） */
} bgp_bmp_cli_ctx_t;

/** 返回默认上下文（空实例名） */
static bgp_bmp_cli_ctx_t bgp_bmp_cli_ctx_default(void)
{
    bgp_bmp_cli_ctx_t c;
    memset(&c, 0, sizeof(c));
    return c;
}

/**
 * @brief 解析单条上下文 TLV 条目，提取 BMP 实例名
 */
static void bgp_bmp_cli_ctx_parse(bgp_bmp_cli_ctx_t *ctx, cli_tlv_entry_t *entry)
{
    if (entry->cfg_id == CLI_CTX_ID_BMP_INST_NAME && entry->type == CLI_TLV_TYPE_CTX_STR)
    {
        uint16_t copy_len = entry->length;
        if (copy_len >= sizeof(ctx->inst_name))
        {
            copy_len = sizeof(ctx->inst_name) - 1;
        }
        memcpy(ctx->inst_name, entry->value, copy_len);
        ctx->inst_name[copy_len] = '\0';
    }
}

// ============================================================================
// 通用响应辅助
// ============================================================================

/**
 * @brief 处理 apply 结果并发送 CLI 响应
 * @return ERRCODE_SUCCESS 成功应用或无变更
 */
static int bmp_apply_respond(dev_ipc_message_t *msg, bgp_apply_cmd_t *apply)
{
    if (apply->rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply->rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply->errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS; /* RC_OK，调用者继续写 DB */
}

// ============================================================================
// Group 14: bmp instance <name> / no bmp instance <name>
// ============================================================================

static int handle_bmp_instance(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_BMP_INSTANCE;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;

    /* 从参数中获取实例名 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1 && entry.value && entry.length > 0)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(apply.u.bmp_instance.name, sizeof(apply.u.bmp_instance.name), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (apply.u.bmp_instance.name[0] == '\0')
    {
        bgp_send_cli_response(msg, "BMP Error: Missing instance name.\r\n");
        return ERRCODE_FAIL;
    }

    /* 派发到 worker 线程 */
    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BMP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    int ret = bmp_apply_respond(msg, &apply);
    if (ret != ERRCODE_SUCCESS || apply.rc != BGP_APPLY_RC_OK)
    {
        return ret;
    }

    /* 写 DB */
    if (apply.isNo)
    {
        bgp_bmp_db_del_instance(apply.u.bmp_instance.name);
    }
    else
    {
        if (bgp_bmp_db_set_instance(apply.u.bmp_instance.name) != 0)
        {
            bgp_send_cli_response(msg, "BMP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

// ============================================================================
// Group 15: collector <ip> port <port> / no collector
// ============================================================================

static int handle_bmp_collector(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_BMP_COLLECTOR;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;

    bgp_bmp_cli_ctx_t ctx = bgp_bmp_cli_ctx_default();
    char ip_buf[64] = {0};
    uint16_t port = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_bmp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1 && entry.value && entry.length > 0)
        {
            uint16_t len = entry.length < sizeof(ip_buf) - 1 ? entry.length : (uint16_t)(sizeof(ip_buf) - 1);
            memcpy(ip_buf, entry.value, len);
            ip_buf[len] = '\0';
        }
        else if (entry.cfg_id == 2)
        {
            port = (uint16_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (ctx.inst_name[0] == '\0')
    {
        bgp_send_cli_response(msg, "BMP Error: Missing instance context.\r\n");
        return ERRCODE_FAIL;
    }

    g_strlcpy(apply.u.bmp_collector.inst_name, ctx.inst_name, sizeof(apply.u.bmp_collector.inst_name));
    apply.u.bmp_collector.port = port;

    if (!apply.isNo)
    {
        if (ip_buf[0] == '\0' || port == 0)
        {
            bgp_send_cli_response(msg, "BMP Error: Missing collector IP or port.\r\n");
            return ERRCODE_FAIL;
        }
        if (net_addr_from_str(ip_buf, &apply.u.bmp_collector.addr) != 0)
        {
            bgp_send_cli_response(msg, "BMP Error: Invalid collector IP address.\r\n");
            return ERRCODE_FAIL;
        }
    }

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BMP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    int ret = bmp_apply_respond(msg, &apply);
    if (ret != ERRCODE_SUCCESS || apply.rc != BGP_APPLY_RC_OK)
    {
        return ret;
    }

    /* 写 DB */
    if (apply.isNo)
    {
        bgp_bmp_db_del_collector(ctx.inst_name);
    }
    else
    {
        if (bgp_bmp_db_set_collector(ctx.inst_name, ip_buf, port) != 0)
        {
            bgp_send_cli_response(msg, "BMP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

// ============================================================================
// Group 16: stats-report interval <seconds> / no stats-report interval
// ============================================================================

static int handle_bmp_stats_interval(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_BMP_STATS_INTERVAL;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;

    bgp_bmp_cli_ctx_t ctx = bgp_bmp_cli_ctx_default();

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_bmp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            apply.u.bmp_stats.interval = (uint16_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (ctx.inst_name[0] == '\0')
    {
        bgp_send_cli_response(msg, "BMP Error: Missing instance context.\r\n");
        return ERRCODE_FAIL;
    }

    g_strlcpy(apply.u.bmp_stats.inst_name, ctx.inst_name, sizeof(apply.u.bmp_stats.inst_name));

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BMP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    int ret = bmp_apply_respond(msg, &apply);
    if (ret != ERRCODE_SUCCESS || apply.rc != BGP_APPLY_RC_OK)
    {
        return ret;
    }

    if (apply.isNo)
    {
        bgp_bmp_db_del_stats_interval(ctx.inst_name);
    }
    else
    {
        bgp_bmp_db_set_stats_interval(ctx.inst_name, apply.u.bmp_stats.interval);
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

// ============================================================================
// Group 17: reconnect interval <seconds> / no reconnect interval
// ============================================================================

static int handle_bmp_reconnect(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_BMP_RECONNECT;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;

    bgp_bmp_cli_ctx_t ctx = bgp_bmp_cli_ctx_default();

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_bmp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            apply.u.bmp_reconnect.interval = (uint16_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (ctx.inst_name[0] == '\0')
    {
        bgp_send_cli_response(msg, "BMP Error: Missing instance context.\r\n");
        return ERRCODE_FAIL;
    }

    g_strlcpy(apply.u.bmp_reconnect.inst_name, ctx.inst_name, sizeof(apply.u.bmp_reconnect.inst_name));

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BMP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    int ret = bmp_apply_respond(msg, &apply);
    if (ret != ERRCODE_SUCCESS || apply.rc != BGP_APPLY_RC_OK)
    {
        return ret;
    }

    if (apply.isNo)
    {
        bgp_bmp_db_del_reconnect_interval(ctx.inst_name);
    }
    else
    {
        bgp_bmp_db_set_reconnect_interval(ctx.inst_name, apply.u.bmp_reconnect.interval);
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

// ============================================================================
// Group 18: monitor neighbor all / monitor neighbor <ip> / no monitor neighbor <ip>
// ============================================================================

static int handle_bmp_monitor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_BMP_MONITOR;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;

    bgp_bmp_cli_ctx_t ctx = bgp_bmp_cli_ctx_default();
    char ip_buf[64] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_bmp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            /* "all" 关键字 */
            apply.u.bmp_monitor.is_all = TRUE;
        }
        else if (entry.cfg_id == 2 && entry.value && entry.length > 0)
        {
            /* IP 地址参数 */
            uint16_t len = entry.length < sizeof(ip_buf) - 1 ? entry.length : (uint16_t)(sizeof(ip_buf) - 1);
            memcpy(ip_buf, entry.value, len);
            ip_buf[len] = '\0';
        }
        cli_tlv_entry_free(&entry);
    }

    if (ctx.inst_name[0] == '\0')
    {
        bgp_send_cli_response(msg, "BMP Error: Missing instance context.\r\n");
        return ERRCODE_FAIL;
    }

    g_strlcpy(apply.u.bmp_monitor.inst_name, ctx.inst_name, sizeof(apply.u.bmp_monitor.inst_name));

    if (!apply.u.bmp_monitor.is_all)
    {
        if (ip_buf[0] == '\0')
        {
            bgp_send_cli_response(msg, "BMP Error: Missing neighbor parameter.\r\n");
            return ERRCODE_FAIL;
        }
        if (net_addr_from_str(ip_buf, &apply.u.bmp_monitor.addr) != 0)
        {
            bgp_send_cli_response(msg, "BMP Error: Invalid neighbor IP address.\r\n");
            return ERRCODE_FAIL;
        }
    }

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BMP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    int ret = bmp_apply_respond(msg, &apply);
    if (ret != ERRCODE_SUCCESS || apply.rc != BGP_APPLY_RC_OK)
    {
        return ret;
    }

    /* 写 DB */
    if (apply.u.bmp_monitor.is_all && !apply.isNo)
    {
        bgp_bmp_db_set_monitor_all(ctx.inst_name);
    }
    else if (apply.isNo && ip_buf[0] != '\0')
    {
        bgp_bmp_db_del_monitor_peer(ctx.inst_name, ip_buf);
    }
    else if (ip_buf[0] != '\0')
    {
        bgp_bmp_db_add_monitor_peer(ctx.inst_name, ip_buf);
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 入口分发
// ============================================================================

int bgp_bmp_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("BMP: 配置命令 payload 解析失败");
        bgp_send_cli_response(msg, "BMP Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("BMP: 配置命令 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case BGP_CLI_GROUP_ID_BMP_INSTANCE:
            result = handle_bmp_instance(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_BMP_COLLECTOR:
            result = handle_bmp_collector(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_BMP_STATS_INTERVAL:
            result = handle_bmp_stats_interval(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_BMP_RECONNECT:
            result = handle_bmp_reconnect(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_BMP_MONITOR:
            result = handle_bmp_monitor(msg, &parser);
            break;
        default:
            LOG_WARN("BMP: 未知配置命令 group_id=%u", parser.group_id);
            bgp_send_cli_response(msg, "BMP Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
