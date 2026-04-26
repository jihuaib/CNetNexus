/**
 * @file   bgp_cli.c
 * @brief  BGP 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#include "bgp_cli.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "bgp_cfg_apply.h"
#include "bgp_conn.h"
#include "bgp_db.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

void bgp_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(bgp_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 上下文序列化辅助（TLV 格式）
// 保留备用，当前视图切换由 CLI 框架自动处理
// ============================================================================

// ============================================================================
// 上下文 ID 定义及解析辅助
// ============================================================================

/* BGP 上下文变量 ID 使用 cli.h 中全局定义的 CLI_CTX_ID_BGP_* */

/** 从 TLV 上下文提取的视图参数，带默认值（公网 VRF + IPv4 单播） */
typedef struct bgp_cli_ctx
{
    uint32_t vrf_id; /**< VRF ID */
    bgp_afi_t afi;   /**< 地址族 */
    bgp_safi_t safi; /**< 子地址族 */
} bgp_cli_ctx_t;

/** 返回默认上下文（公网 VRF + IPv4 单播） */
static bgp_cli_ctx_t bgp_cli_ctx_default(void)
{
    bgp_cli_ctx_t c = {BGP_VRF_PUBLIC_ID, BGP_AFI_IPV4, BGP_SAFI_UNICAST};
    return c;
}

/**
 * @brief 解析单条 CONTEXT TLV 条目并更新上下文结构
 * @param ctx   待更新的上下文
 * @param entry 当前上下文 TLV 条目（调用者确保 CLI_TLV_IS_CTX 为真）
 */
static void bgp_cli_ctx_parse(bgp_cli_ctx_t *ctx, cli_tlv_entry_t *entry)
{
    /* entry->cfg_id 即 ctx_id（独立命名空间），type == CLI_TLV_TYPE_CTX 已由调用者检查 */
    switch (entry->cfg_id)
    {
        case CLI_CTX_ID_BGP_VRF:
            ctx->vrf_id = cli_tlv_entry_get_ctx_uint32(entry);
            break;
        case CLI_CTX_ID_BGP_AFI:
            ctx->afi = (bgp_afi_t)cli_tlv_entry_get_ctx_uint32(entry);
            break;
        case CLI_CTX_ID_BGP_SAFI:
            ctx->safi = (bgp_safi_t)cli_tlv_entry_get_ctx_uint32(entry);
            break;
        default:
            break;
    }
}

/**
 * @brief 将 AFI/SAFI 组合转换为字符串（用于数据库写入）
 */
static const char *bgp_af_str(bgp_afi_t afi, bgp_safi_t safi)
{
    if (afi == BGP_AFI_IPV4 && safi == BGP_SAFI_UNICAST)
    {
        return "ipv4-unicast";
    }
    if (afi == BGP_AFI_IPV6 && safi == BGP_SAFI_UNICAST)
    {
        return "ipv6-unicast";
    }
    if (afi == BGP_AFI_IPV4 && safi == BGP_SAFI_QP)
    {
        return "ipv4-qp";
    }
    if (afi == BGP_AFI_IPV6 && safi == BGP_SAFI_QP)
    {
        return "ipv6-qp";
    }
    return "unknown";
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理 bgp 配置命令（bgp <as-number> / no bgp [as-number]）
 *
 * group_id=1, cfg_id: 2=as_number
 */
static int handle_bgp_protocol(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_PROTOCOL;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    apply.vrf_id = BGP_VRF_PUBLIC_ID;

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
            apply.u.protocol.as_number = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (!(apply.isNo) && apply.u.protocol.as_number == 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing AS number.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    /* 应用成功，写 DB */
    if (apply.isNo)
    {
        if (bgp_db_del_as() < 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database cleanup failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    else
    {
        if (bgp_db_set_as(apply.u.protocol.as_number) != 0 ||
            bgp_db_set_vrf_router_id(BGP_VRF_PUBLIC_ID, "0.0.0.0") != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> as <as-num> / no neighbor <ip> 命令
 *
 * group_id=2, cfg_id: 1=ipv4-address, 2=ipv6-address, 3=as-number
 */
static int handle_bgp_neighbor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_NEIGHBOR;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    char ip_buf[64] = {0};
    int has_remote_as = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1:
            case 2:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", s);
                }
                break;
            }
            case 3:
                apply.u.neighbor.remote_as = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_remote_as = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    /* 基本参数校验（无需访问 server 线程数据） */
    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (net_addr_from_str(ip_buf, &apply.u.neighbor.addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!(apply.isNo) && !has_remote_as)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing remote AS number.\r\n");
        return ERRCODE_FAIL;
    }
    apply.vrf_id = ctx.vrf_id;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    /* 写 DB */
    char resp_buf[CLI_MAX_RESP_LEN];
    resp_buf[0] = '\0';
    if (apply.isNo)
    {
        int rows = bgp_db_del_session(ctx.vrf_id, ip_buf);
        if (rows < 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database cleanup failed.\r\n");
            return ERRCODE_FAIL;
        }
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s deleted (%d row).\r\n", ip_buf, rows > 0 ? rows : 0);
    }
    else
    {
        if (bgp_db_set_session(ctx.vrf_id, ip_buf, apply.u.neighbor.remote_as) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s AS %u configured.\r\n", ip_buf,
                 apply.u.neighbor.remote_as);
    }
    bgp_send_cli_response(msg, resp_buf);
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 af ipv4-unicast / no af ipv4-unicast 命令
 *
 * group_id=4, cfg_id: 1=ipv4-unicast (keyword)
 */
static int handle_bgp_addr_family(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_ADDR_FAMILY;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_UNICAST;
                break;
            case 2:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_UNICAST;
                break;
            case 3:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_QP;
                break;
            case 4:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_QP;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    apply.vrf_id = ctx.vrf_id;
    apply.u.instance.afi = ctx.afi;
    apply.u.instance.safi = ctx.safi;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP && !apply.isNo)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK && !(apply.isNo && apply.rc == BGP_APPLY_RC_NOOP))
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    /* 写 DB */
    if (apply.isNo)
    {
        if (bgp_db_del_instance(ctx.vrf_id, ctx.afi, ctx.safi) < 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database cleanup failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    else
    {
        if (bgp_db_set_instance(ctx.vrf_id, ctx.afi, ctx.safi) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> enable / no neighbor <ip> 命令（地址族视图）
 *
 * group_id=4, cfg_id: 1=ipv4-address, 2=ipv6-address, 3=enable (keyword)
 */
static int handle_bgp_af_neighbor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_AF_NEIGHBOR;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    char ip_buf[64] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1 || entry.cfg_id == 2)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(ip_buf, sizeof(ip_buf), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (net_addr_from_str(ip_buf, &apply.u.af_neighbor.addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }
    apply.vrf_id = ctx.vrf_id;
    apply.u.af_neighbor.afi = ctx.afi;
    apply.u.af_neighbor.safi = ctx.safi;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    /* 写 DB */
    const char *af_name = bgp_af_str(ctx.afi, ctx.safi);
    char resp_buf[CLI_MAX_RESP_LEN];
    resp_buf[0] = '\0';
    if (apply.isNo)
    {
        int rows = bgp_db_del_neighbor(ctx.vrf_id, ip_buf, ctx.afi, ctx.safi);
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s disabled for %s (%d row).\r\n", ip_buf, af_name,
                 rows > 0 ? rows : 0);
    }
    else
    {
        if (bgp_db_set_neighbor(ctx.vrf_id, ip_buf, ctx.afi, ctx.safi) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s enabled for %s.\r\n", ip_buf, af_name);
    }
    bgp_send_cli_response(msg, resp_buf);
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 "timer keepalive <n> hold <n>" / "no timer keepalive" 命令
 *
 * group_id=7, cfg_id: 1=keepalive-time, 2=hold-time
 */
static int handle_bgp_timers(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_TIMERS;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    uint32_t keepalive = 0, hold_time = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1:
                keepalive = (uint32_t)cli_tlv_entry_get_int(&entry);
                break;
            case 2:
                hold_time = (uint32_t)cli_tlv_entry_get_int(&entry);
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!(apply.isNo) && (keepalive == 0 || hold_time == 0))
    {
        bgp_send_cli_response(msg, "BGP Error: Missing timer parameters.\r\n");
        return ERRCODE_FAIL;
    }
    if (!(apply.isNo) && hold_time <= keepalive)
    {
        bgp_send_cli_response(msg, "BGP Error: Hold time must be greater than keepalive time.\r\n");
        return ERRCODE_FAIL;
    }
    apply.vrf_id = ctx.vrf_id;
    apply.u.timers.keepalive = (uint16_t)keepalive;
    apply.u.timers.hold_time = (uint16_t)hold_time;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    /* 写 DB */
    if (apply.isNo)
    {
        (void)bgp_db_del_vrf_timers(ctx.vrf_id);
    }
    else
    {
        if (bgp_db_set_vrf_timers(ctx.vrf_id, apply.u.timers.keepalive, apply.u.timers.hold_time) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 router-id <ip-address> / no router-id 命令
 *
 * group_id=6, cfg_id: 1=ip-address
 */
static int handle_bgp_router_id(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_ROUTER_ID;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(apply.u.router_id.id, sizeof(apply.u.router_id.id), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (!(apply.isNo))
    {
        if (apply.u.router_id.id[0] == '\0')
        {
            bgp_send_cli_response(msg, "BGP Error: Missing router-id IP address.\r\n");
            return ERRCODE_FAIL;
        }
        struct in_addr addr;
        if (inet_pton(AF_INET, apply.u.router_id.id, &addr) != 1)
        {
            bgp_send_cli_response(msg, "BGP Error: Invalid IPv4 address.\r\n");
            return ERRCODE_FAIL;
        }
    }
    apply.vrf_id = ctx.vrf_id;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    /* 写 DB */
    if (apply.isNo)
    {
        (void)bgp_db_del_vrf_router_id(ctx.vrf_id);
    }
    else
    {
        if (bgp_db_set_vrf_router_id(ctx.vrf_id, apply.u.router_id.id) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 "timer connect-retry <n>" / "no timer connect-retry" 命令
 *
 * group_id=8, cfg_id: 1=connect-retry-time
 */
static int handle_bgp_connect_retry(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_CONNECT_RETRY;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    uint32_t connect_retry = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            connect_retry = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (!(apply.isNo) && connect_retry == 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing connect-retry parameter.\r\n");
        return ERRCODE_FAIL;
    }
    apply.vrf_id = ctx.vrf_id;
    apply.u.connect_retry.interval = (uint16_t)connect_retry;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    /* 写 DB */
    if (apply.isNo)
    {
        (void)bgp_db_del_vrf_connect_retry(ctx.vrf_id);
    }
    else
    {
        if (bgp_db_set_vrf_connect_retry(ctx.vrf_id, apply.u.connect_retry.interval) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> open-capability as4|route-refresh 命令
 *
 * group_id=8, cfg_id: 1=ipv4-address, 2=ipv6-address, 3=as4 (keyword), 4=route-refresh (keyword)
 * is_no=TRUE 时关闭对应能力，否则开启。
 */
static int handle_bgp_open_capability(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_OPEN_CAP;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    char ip_buf[64] = {0};
    gboolean has_as4 = FALSE;
    gboolean has_rr = FALSE;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1: /* <ipv4-address> 参数 */
            case 2: /* <ipv6-address> 参数 */
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", s);
                }
                break;
            }
            case 3: /* as4 关键字 */
                has_as4 = TRUE;
                break;
            case 4: /* route-refresh 关键字 */
                has_rr = TRUE;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!has_as4 && !has_rr)
    {
        bgp_send_cli_response(msg, "BGP Error: Unknown capability.\r\n");
        return ERRCODE_FAIL;
    }
    if (net_addr_from_str(ip_buf, &apply.u.open_cap.addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }

    apply.vrf_id = ctx.vrf_id;
    apply.u.open_cap.cap_bit = has_rr ? BGP_SESS_CAP_ROUTE_REFRESH : BGP_SESS_CAP_AS4;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    /* 写 DB */
    if (bgp_db_set_session_caps(ctx.vrf_id, ip_buf, apply.out.sess_flags) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

// ============================================================================
// Group 11: import-route 协议导入命令
//
// cfg-id 映射：1=no, 2=static
// ============================================================================

static int handle_bgp_import_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_IMPORT_ROUTE;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t bctx = bgp_cli_ctx_default();
    int has_static = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&bctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1: /* static 关键字 */
                has_static = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_static)
    {
        bgp_send_cli_response(msg, "Error: Must specify import route type\r\n");
        return ERRCODE_FAIL;
    }

    apply.vrf_id = bctx.vrf_id;
    apply.u.import_route.afi = bctx.afi;
    apply.u.import_route.safi = bctx.safi;
    apply.u.import_route.import_proto = ROUTE_PROTOCOL_STATIC;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    /* 写 DB */
    bgp_db_set_import_protos(bctx.vrf_id, bctx.afi, bctx.safi, apply.out.import_protos);

    /* 向 ROUTE 模块发送订阅/取消订阅（fire-and-forget） */
    route_subscribe_req_t *req = (route_subscribe_req_t *)g_malloc(sizeof(route_subscribe_req_t));
    req->protocol = ROUTE_PROTOCOL_STATIC;
    req->vrf_id = ROUTE_VRF_DEFAULT;
    req->flags = (apply.isNo) ? 0u : ROUTE_SUBSCRIBE_FLAG_FULL;
    uint32_t sub_type = (apply.isNo) ? ROUTE_MSG_TYPE_UNSUBSCRIBE : ROUTE_MSG_TYPE_SUBSCRIBE;
    dev_ipc_message_t *sub_msg = dev_ipc_message_create(sub_type, DEV_MODULE_ID_BGP, DEV_MODULE_ID_ROUTE, 0, req,
                                                        sizeof(route_subscribe_req_t), g_free);
    if (sub_msg)
    {
        if (dev_ipc_send(bgp_local_ipc_ctx(), DEV_MODULE_ID_ROUTE, sub_msg) != 0)
        {
            LOG_WARN("BGP: Failed to send route %s request (ROUTE module may not be ready)",
                     (apply.isNo) ? "unsubscribe" : "subscribe");
        }
        dev_ipc_message_free(sub_msg);
    }

    bgp_send_cli_response(msg, (apply.isNo) ? "import-route static disabled\r\n" : "import-route static enabled\r\n");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> source-interface <if-name> / no neighbor <ip> source-interface
 *
 * group_id=12, cfg_id: 1=ipv4-address, 2=ipv6-address, 3=if-name
 */
static int handle_bgp_source_interface(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_SOURCE_IF;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    char ip_buf[64] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1:
            case 2:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", s);
                }
                break;
            }
            case 3:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(apply.u.source_if.if_name, sizeof(apply.u.source_if.if_name), "%s", s);
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (net_addr_from_str(ip_buf, &apply.u.source_if.addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!apply.isNo && apply.u.source_if.if_name[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing source interface name.\r\n");
        return ERRCODE_FAIL;
    }
    apply.vrf_id = ctx.vrf_id;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP && !apply.isNo)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK && !(apply.isNo && apply.rc == BGP_APPLY_RC_NOOP))
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    if (apply.isNo)
    {
        if (bgp_db_del_session_source_if(ctx.vrf_id, ip_buf) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    else
    {
        if (bgp_db_set_session_source_if(ctx.vrf_id, ip_buf, apply.u.source_if.if_name) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> ebgp-multihop <ttl> / no neighbor <ip> ebgp-multihop
 *
 * group_id=13, cfg_id: 1=ipv4-address, 2=ipv6-address, 3=ttl
 */
static int handle_bgp_ebgp_multihop(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_EBGP_MULTIHOP;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    char ip_buf[64] = {0};
    uint32_t ttl = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1:
            case 2:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", s);
                }
                break;
            }
            case 3:
                ttl = (uint32_t)cli_tlv_entry_get_int(&entry);
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (net_addr_from_str(ip_buf, &apply.u.ebgp_multihop.addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!apply.isNo && (ttl == 0 || ttl > 255))
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid ebgp-multihop TTL (1-255).\r\n");
        return ERRCODE_FAIL;
    }
    apply.vrf_id = ctx.vrf_id;
    apply.u.ebgp_multihop.ttl = (uint8_t)ttl;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }

    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    if (apply.isNo)
    {
        if (bgp_db_del_session_ebgp_multihop(ctx.vrf_id, ip_buf) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    else
    {
        if (bgp_db_set_session_ebgp_multihop(ctx.vrf_id, ip_buf, apply.u.ebgp_multihop.ttl) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 QP 自产生路由 route start-dqpn ... / no route start-dqpn ...
 *
 * group_id=16, cfg_id: 1=dqpn, 2=ipv4-addr, 3=ipv6-addr, 4=mask, 5=count, 6=bid
 */
static int handle_bgp_qp_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_QP_ROUTE;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    char ip_buf[64] = {0};
    char bid_buf[64] = {0};
    uint32_t dqpn = 0;
    uint32_t count = 0;
    uint32_t mask = 0;
    int has_dqpn = 0;
    int has_count = 0;
    int has_mask = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1:
                dqpn = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_dqpn = 1;
                break;
            case 2:
            case 3:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", s);
                }
                break;
            }
            case 4:
                mask = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_mask = 1;
                break;
            case 5:
                count = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_count = 1;
                break;
            case 6:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(bid_buf, sizeof(bid_buf), "%s", s);
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_dqpn || !has_count || !has_mask || ip_buf[0] == '\0' || bid_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing QP route parameters.\r\n");
        return ERRCODE_FAIL;
    }
    if (net_addr_from_str(ip_buf, &apply.u.qp_route.ip) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid prefix address.\r\n");
        return ERRCODE_FAIL;
    }
    if (net_addr_from_str(bid_buf, &apply.u.qp_route.bid) != 0 || apply.u.qp_route.bid.family != AF_INET6)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid BID IPv6 address.\r\n");
        return ERRCODE_FAIL;
    }
    if (ctx.safi != BGP_SAFI_QP)
    {
        bgp_send_cli_response(msg, "BGP Error: QP address family required.\r\n");
        return ERRCODE_FAIL;
    }

    apply.vrf_id = ctx.vrf_id;
    apply.u.qp_route.afi = ctx.afi;
    apply.u.qp_route.safi = ctx.safi;
    apply.u.qp_route.start_dqpn = dqpn;
    apply.u.qp_route.count = count;
    apply.u.qp_route.mask_len = (uint8_t)mask;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }
    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    char prefix_buf[64] = {0};
    char bid_db_buf[64] = {0};
    net_addr_to_str(&apply.u.qp_route.ip, prefix_buf, sizeof(prefix_buf));
    net_addr_to_str(&apply.u.qp_route.bid, bid_db_buf, sizeof(bid_db_buf));

    if (apply.isNo)
    {
        if (bgp_db_del_qp_route(ctx.vrf_id, apply.u.qp_route.afi, apply.u.qp_route.safi, apply.u.qp_route.start_dqpn,
                                apply.u.qp_route.count, prefix_buf, apply.u.qp_route.mask_len, bid_db_buf) < 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database cleanup failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    else
    {
        if (bgp_db_set_qp_route(ctx.vrf_id, apply.u.qp_route.afi, apply.u.qp_route.safi, apply.u.qp_route.start_dqpn,
                                apply.u.qp_route.count, prefix_buf, apply.u.qp_route.mask_len, bid_db_buf) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 route-select enable / no route-select enable
 *
 * group_id=17，无 cfg-id 参数。
 */
static int handle_bgp_route_select(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.group_id = BGP_CLI_GROUP_ID_ROUTE_SELECT;
    apply.isNo = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (ctx.safi != BGP_SAFI_QP)
    {
        bgp_send_cli_response(msg, "BGP Error: QP address family required.\r\n");
        return ERRCODE_FAIL;
    }

    apply.vrf_id = ctx.vrf_id;
    apply.u.route_select.afi = ctx.afi;
    apply.u.route_select.safi = ctx.safi;

    if (bgp_worker_dispatch_apply(&apply) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Server unavailable.\r\n");
        return ERRCODE_FAIL;
    }
    if (apply.rc == BGP_APPLY_RC_NOOP)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (apply.rc != BGP_APPLY_RC_OK)
    {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s\r\n", apply.errmsg);
        bgp_send_cli_response(msg, buf);
        return ERRCODE_FAIL;
    }

    if (bgp_db_set_route_select(ctx.vrf_id, apply.u.route_select.afi, apply.u.route_select.safi, !apply.isNo) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理配置类 CLI 命令（group 1-8, 11-13），在 IPC worker 线程调用
 *
 * 配置命令通过 bgp_worker_dispatch_apply() 将状态变更派发到 BGP worker 线程，
 * 然后在 IPC worker 线程完成 DB 写入并发送响应。
 */
int bgp_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("BGP: 配置命令 payload 解析失败");
        bgp_send_cli_response(msg, "BGP Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("BGP: 配置命令 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case BGP_CLI_GROUP_ID_PROTOCOL:
            result = handle_bgp_protocol(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_NEIGHBOR:
            result = handle_bgp_neighbor(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_ADDR_FAMILY:
            result = handle_bgp_addr_family(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_AF_NEIGHBOR:
            result = handle_bgp_af_neighbor(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_ROUTER_ID:
            result = handle_bgp_router_id(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_TIMERS:
            result = handle_bgp_timers(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_CONNECT_RETRY:
            result = handle_bgp_connect_retry(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_OPEN_CAP:
            result = handle_bgp_open_capability(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_IMPORT_ROUTE:
            result = handle_bgp_import_route(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_SOURCE_IF:
            result = handle_bgp_source_interface(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_EBGP_MULTIHOP:
            result = handle_bgp_ebgp_multihop(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_QP_ROUTE:
            result = handle_bgp_qp_route(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_ROUTE_SELECT:
            result = handle_bgp_route_select(msg, &parser);
            break;
        default:
            LOG_WARN("BGP: 未知配置命令 group_id=%u", parser.group_id);
            bgp_send_cli_response(msg, "BGP Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
