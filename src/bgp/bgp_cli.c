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
#include <time.h>

#include "bgp_cfg_apply.h"
#include "bgp_conn.h"
#include "bgp_db.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
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

static void bgp_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_bgp_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

int bgp_cli_send_chunked_response(dev_ipc_message_t *msg, GString *full_text)
{
    return cli_chunk_stream_start(&g_bgp_local->show_stream, g_bgp_local->dev_ipc_ctx, DEV_MODULE_ID_BGP, msg,
                                  full_text);
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
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t as_number = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 2)
        {
            as_number = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (!is_no)
    {
        if (g_bgp_local->protocol != NULL)
        {
            if (g_bgp_local->protocol->as_number == as_number)
            {
                bgp_send_cli_response(msg, "");
                return ERRCODE_SUCCESS;
            }
            else
            {
                bgp_send_cli_response(msg, "BGP Error: AS number mismatch.\r\n");
                return ERRCODE_FAIL;
            }
        }
    }
    else
    {
        if (g_bgp_local->protocol == NULL)
        {
            bgp_send_cli_response(msg, "");
            return ERRCODE_SUCCESS;
        }
    }

    if (!is_no && as_number == 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing AS number.\r\n");
        return ERRCODE_FAIL;
    }

    uint32_t apply_ret = bgp_cfg_apply_protocol(is_no, as_number);
    if (apply_ret != ERRCODE_SUCCESS)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to apply protocol configuration.\r\n");
        return ERRCODE_FAIL;
    }

    /* no bgp：内存删除成功后再清理持久化 */
    if (is_no)
    {
        (void)bgp_db_del_as(g_bgp_local->dev_ipc_ctx);
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (bgp_db_set_as(g_bgp_local->dev_ipc_ctx, as_number) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_db_set_vrf_router_id(g_bgp_local->dev_ipc_ctx, BGP_VRF_PUBLIC_ID, "0.0.0.0") != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> as <as-num> / no neighbor <ip> 命令
 *
 * group_id=3, cfg_id: 2=ip-address, 3=as-number
 */
static int handle_bgp_neighbor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t remote_as = 0;
    int has_remote_as = 0;
    char ip_buf[64] = {0};
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
            case 2: /* ip-address 参数 */
            {
                const char *ip_str = cli_tlv_entry_get_text(&entry);
                if (ip_str)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", ip_str);
                }
                break;
            }
            case 3: /* as-number 参数 */
                remote_as = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_remote_as = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    resp_buf[0] = '\0';

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured. Run 'bgp <as-number>' first.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);

    net_addr_t ip_addr;
    gboolean has_valid_ip = (ip_buf[0] != '\0' && net_addr_from_str(ip_buf, &ip_addr) == 0);

    /* 先做同配置短路 */
    if (is_no)
    {
        bgp_session_t *existing = (has_valid_ip && vrf) ? bgp_vrf_find_session(vrf, &ip_addr) : NULL;
        if (!existing)
        {
            bgp_send_cli_response(msg, "");
            return ERRCODE_SUCCESS;
        }
    }
    else
    {
        bgp_session_t *existing = (has_valid_ip && has_remote_as && vrf) ? bgp_vrf_find_session(vrf, &ip_addr) : NULL;
        if (existing && existing->remote_as == remote_as)
        {
            bgp_send_cli_response(msg, "");
            return ERRCODE_SUCCESS;
        }
    }

    /* 参数校验 */
    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!has_valid_ip)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!is_no && !has_remote_as)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing remote AS number.\r\n");
        return ERRCODE_FAIL;
    }

    uint32_t apply_ret = bgp_cfg_apply_neighbor(is_no, vrf, &ip_addr, remote_as);
    if (apply_ret != ERRCODE_SUCCESS)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to apply neighbor configuration.\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        int rows = bgp_db_del_session(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ip_buf);
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s deleted (%d row).\r\n", ip_buf, rows > 0 ? rows : 0);
        bgp_send_cli_response(msg, resp_buf);
        return ERRCODE_SUCCESS;
    }

    if (bgp_db_set_session(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ip_buf, remote_as) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s AS %u configured.\r\n", ip_buf, remote_as);
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
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
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
        /* 命令参数：cfg-id=1 → ipv4-unicast，后续可扩展更多 AF */
        switch (entry.cfg_id)
        {
            case 1: /* ipv4-unicast 关键字 */
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_UNICAST;
                break;
            case 2: /* ipv6-unicast 关键字 */
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_UNICAST;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);
    if (!vrf)
    {
        bgp_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));

    /* 先做同配置短路 */
    if (is_no && !inst)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (!is_no && inst)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    uint32_t apply_ret = bgp_cfg_apply_instance(is_no, vrf, ctx.afi, ctx.safi);
    if (apply_ret != ERRCODE_SUCCESS)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to apply instance configuration.\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        bgp_db_del_neighbors_by_afi(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ctx.afi, ctx.safi);
        bgp_db_del_instance(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ctx.afi, ctx.safi);
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (bgp_db_set_instance(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ctx.afi, ctx.safi) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> enable / no neighbor <ip> 命令（地址族视图）
 *
 * group_id=5, cfg_id: 2=ip-address, 3=enable (keyword)
 */
static int handle_bgp_af_neighbor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char ip_buf[64] = {0};
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
            case 2: /* ip-address 参数 */
            {
                const char *ip_str = cli_tlv_entry_get_text(&entry);
                if (ip_str)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", ip_str);
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    resp_buf[0] = '\0';

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);
    const char *af_name = bgp_af_str(ctx.afi, ctx.safi);

    net_addr_t ip_addr;
    gboolean has_valid_ip = (ip_buf[0] != '\0' && net_addr_from_str(ip_buf, &ip_addr) == 0);
    gboolean af_enabled = FALSE;
    if (has_valid_ip && vrf)
    {
        char addr_key[64];
        net_addr_to_str(&ip_addr, addr_key, sizeof(addr_key));
        bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));
        af_enabled = (inst && inst->peer_hash && g_hash_table_lookup(inst->peer_hash, addr_key));
    }

    /* 先做同配置短路 */
    if (is_no && !af_enabled)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (!is_no && af_enabled)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* 参数校验 */
    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!has_valid_ip)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!vrf)
    {
        bgp_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }
    if (!is_no && !bgp_vrf_find_session(vrf, &ip_addr))
    {
        bgp_send_cli_response(msg,
                              "BGP Error: Neighbor session not configured. Run 'neighbor <ip> as <as>' first.\r\n");
        return ERRCODE_FAIL;
    }

    uint32_t apply_ret = bgp_cfg_apply_af_neighbor(is_no, vrf, ctx.afi, ctx.safi, &ip_addr);
    if (apply_ret != ERRCODE_SUCCESS)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to apply AF neighbor configuration.\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        int rows = bgp_db_del_neighbor(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ip_buf, ctx.afi, ctx.safi);
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s disabled for %s (%d row).\r\n", ip_buf, af_name,
                 rows > 0 ? rows : 0);
        bgp_send_cli_response(msg, resp_buf);
        return ERRCODE_SUCCESS;
    }

    if (bgp_db_set_neighbor(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ip_buf, ctx.afi, ctx.safi) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s enabled for %s.\r\n", ip_buf, af_name);
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
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t keepalive = 0;
    uint32_t hold_time = 0;
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

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);
    if (!vrf)
    {
        bgp_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }

    /* 先做同配置短路 */
    if (is_no && vrf->keepalive == BGP_TIMER_DEFAULT_KEEPALIVE && vrf->hold_time == BGP_TIMER_DEFAULT_HOLD)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (!is_no && keepalive == vrf->keepalive && hold_time == vrf->hold_time)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* 参数校验 */
    if (!is_no && (keepalive == 0 || hold_time == 0))
    {
        bgp_send_cli_response(msg, "BGP Error: Missing timer parameters.\r\n");
        return ERRCODE_FAIL;
    }
    if (!is_no && hold_time <= keepalive)
    {
        bgp_send_cli_response(msg, "BGP Error: Hold time must be greater than keepalive time.\r\n");
        return ERRCODE_FAIL;
    }

    uint32_t apply_ret = bgp_cfg_apply_timers(is_no, vrf, (uint16_t)keepalive, (uint16_t)hold_time);
    if (apply_ret != ERRCODE_SUCCESS)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to apply timer configuration.\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        (void)bgp_db_del_vrf_timers(g_bgp_local->dev_ipc_ctx, ctx.vrf_id);
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (bgp_db_set_vrf_timers(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, (uint16_t)keepalive, (uint16_t)hold_time) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
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
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char ip_buf[16] = {0}; /* IPv4 最长 15 字符 + '\0' */
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
                snprintf(ip_buf, sizeof(ip_buf), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);
    if (!vrf)
    {
        bgp_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }

    /* 先做同配置短路 */
    if (is_no && vrf->router_id == 0)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (!is_no && ip_buf[0] != '\0')
    {
        struct in_addr _rid_cmp;
        if (inet_pton(AF_INET, ip_buf, &_rid_cmp) == 1 && ntohl(_rid_cmp.s_addr) == vrf->router_id)
        {
            bgp_send_cli_response(msg, "");
            return ERRCODE_SUCCESS;
        }
    }

    /* 参数校验 */
    if (!is_no && ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing router-id IP address.\r\n");
        return ERRCODE_FAIL;
    }

    struct in_addr addr;
    if (!is_no && inet_pton(AF_INET, ip_buf, &addr) != 1)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IPv4 address.\r\n");
        return ERRCODE_FAIL;
    }

    uint32_t apply_ret = bgp_cfg_apply_router_id(is_no, vrf, ip_buf);
    if (apply_ret != ERRCODE_SUCCESS)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to apply router-id configuration.\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        (void)bgp_db_del_vrf_router_id(g_bgp_local->dev_ipc_ctx, ctx.vrf_id);
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (bgp_db_set_vrf_router_id(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ip_buf) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
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
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t connect_retry = 0;
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
            connect_retry = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);
    if (!vrf)
    {
        bgp_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }

    /* 先做同配置短路 */
    if (is_no && vrf->connect_retry == BGP_TIMER_DEFAULT_CONNECT_RETRY)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (!is_no && connect_retry == vrf->connect_retry)
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* 参数校验 */
    if (!is_no && connect_retry == 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing connect-retry parameter.\r\n");
        return ERRCODE_FAIL;
    }

    uint32_t apply_ret = bgp_cfg_apply_connect_retry(is_no, vrf, (uint16_t)connect_retry);
    if (apply_ret != ERRCODE_SUCCESS)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to apply connect-retry configuration.\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        (void)bgp_db_del_vrf_connect_retry(g_bgp_local->dev_ipc_ctx, ctx.vrf_id);
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (bgp_db_set_vrf_connect_retry(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, (uint16_t)connect_retry) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> open-capability as4|route-refresh 命令
 *
 * group_id=9, cfg_id: 1=ip-address, 2=as4 (keyword), 3=route-refresh (keyword)
 * is_no=TRUE 时关闭对应能力，否则开启。
 */
static int handle_bgp_open_capability(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char ip_buf[64] = {0};
    gboolean has_as4 = FALSE;
    gboolean has_rr = FALSE;
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        /* 解析父视图上下文变量（如 vrf_id） */
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }

        /* entry.cfg_id 即 XML 中的 cfg-id 值 */
        switch (entry.cfg_id)
        {
            case 1: /* <ip-address> 参数 */
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", s);
                }
                break;
            }
            case 2: /* as4 关键字 */
                has_as4 = TRUE;
                break;
            case 3: /* route-refresh 关键字 */
                has_rr = TRUE;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
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

    net_addr_t ip_addr;
    if (net_addr_from_str(ip_buf, &ip_addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);
    bgp_session_t *sess = vrf ? bgp_vrf_find_session(vrf, &ip_addr) : NULL;
    if (!sess)
    {
        bgp_send_cli_response(msg, "BGP Error: Neighbor session not found.\r\n");
        return ERRCODE_FAIL;
    }

    /* 计算目标能力位 */
    uint32_t cap_bit = 0;
    const char *cap_name = "";
    if (has_as4)
    {
        cap_bit = BGP_SESS_CAP_AS4;
        cap_name = "as4";
    }
    else
    {
        cap_bit = BGP_SESS_CAP_ROUTE_REFRESH;
        cap_name = "route-refresh";
    }

    /* 先做同配置短路 */
    if (is_no && !BIT_TEST(sess->flags, cap_bit))
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (!is_no && BIT_TEST(sess->flags, cap_bit))
    {
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    uint32_t apply_ret = bgp_cfg_apply_open_capability(is_no, sess, cap_bit);
    if (apply_ret != ERRCODE_SUCCESS)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to apply capability configuration.\r\n");
        return ERRCODE_FAIL;
    }

    /* 持久化到数据库 */
    if (bgp_db_set_session_caps(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ip_buf, sess->flags) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s open-capability %s %s.\r\n", ip_buf, cap_name,
             is_no ? "disabled" : "enabled");
    bgp_send_cli_response(msg, resp_buf);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 连接状态描述辅助
// ============================================================================

/** 返回 session 当前状态字符串 */
static const char *sess_state_str(const bgp_session_t *sess)
{
    const bgp_conn_t *conn = sess->pri_conn ? sess->pri_conn : sess->sec_conn;
    if (!conn || conn->fd == -1)
    {
        return "Idle";
    }
    if (conn->is_connecting)
    {
        return "Connect";
    }
    switch (sess->state)
    {
        case BGP_CONN_STATE_OPEN_SENT:
            return "OpenSent";
        case BGP_CONN_STATE_OPEN_CONFIRM:
            return "OpenConfirm";
        case BGP_CONN_STATE_ESTABLISHED:
            return "Established";
        default:
            return "Unknown";
    }
}

/** 返回能力位对应的可读字符串 */
static const char *cap_yn(uint32_t caps, uint32_t bit)
{
    return BIT_TEST(caps, bit) ? "Yes" : "No";
}

/** ORIGIN 可读字符串 */
static const char *bgp_origin_str(bgp_origin_t origin)
{
    switch (origin)
    {
        case BGP_ORIGIN_IGP:
            return "IGP";
        case BGP_ORIGIN_EGP:
            return "EGP";
        case BGP_ORIGIN_INCOMPLETE:
            return "INCOMPLETE";
        default:
            return "UNKNOWN";
    }
}

/** 将 nexthop 结构格式化为单行文本 */
static void bgp_nexthop_to_str(const bgp_nexthop_t *nexthop, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }

    char global[64] = "-";
    if (nexthop && nexthop->global.family != 0)
    {
        net_addr_to_str(&nexthop->global, global, sizeof(global));
    }

    if (nexthop && nexthop->has_link_local && nexthop->link_local.family != 0)
    {
        char ll[64];
        net_addr_to_str(&nexthop->link_local, ll, sizeof(ll));
        snprintf(buf, sz, "%s (ll:%s)", global, ll);
        return;
    }

    snprintf(buf, sz, "%s", global);
}

/* 路由表固定列宽 */
#define BGP_RT_COL_NET 24
#define BGP_RT_COL_NH 20
#define BGP_RT_COL_LP 8
#define BGP_RT_COL_MED 8
#define BGP_RT_COL_ORIG 12

/** 将 usec 时间戳格式化为本地时间字符串 */
static void bgp_fmt_time_usec(gint64 usec, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    if (usec <= 0)
    {
        snprintf(buf, sz, "-");
        return;
    }
    time_t sec = (time_t)(usec / 1000000);
    struct tm tmv;
    if (!localtime_r(&sec, &tmv))
    {
        snprintf(buf, sz, "-");
        return;
    }
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tmv);
}

typedef struct bgp_show_route_ctx
{
    GString *buf;
    uint32_t listed_heads;
    uint32_t listed_routes;
} bgp_show_route_ctx_t;

/**
 * @brief 将单条路径的各字段格式化到 lp/med/as_path 缓冲区
 */
static void bgp_route_fmt_fields(const bgp_route_node_t *route, char *lp, size_t lp_sz, char *med, size_t med_sz,
                                 char *as_path, size_t as_sz)
{
    if (route->attr.has_local_pref)
    {
        snprintf(lp, lp_sz, "%u", route->attr.local_pref);
    }
    else
    {
        snprintf(lp, lp_sz, "-");
    }
    if (route->attr.has_med)
    {
        snprintf(med, med_sz, "%u", route->attr.med);
    }
    else
    {
        snprintf(med, med_sz, "-");
    }
    if (route->attr.as_path[0] != '\0')
    {
        snprintf(as_path, as_sz, "%.*s", (int)(as_sz - 1), route->attr.as_path);
    }
    else
    {
        snprintf(as_path, as_sz, "-");
    }
}

static gboolean bgp_show_route_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    bgp_rthead_t *head = (bgp_rthead_t *)value;
    bgp_show_route_ctx_t *ctx = (bgp_show_route_ctx_t *)user_data;
    if (!head || !ctx)
    {
        return FALSE;
    }

    const char *prefix_str = head->nlri.key;
    gboolean first = TRUE;

    GHashTableIter iter;
    gpointer rkey, rval;
    g_hash_table_iter_init(&iter, head->route_hash);
    while (g_hash_table_iter_next(&iter, &rkey, &rval))
    {
        (void)rkey;
        bgp_route_node_t *route = (bgp_route_node_t *)rval;
        if (!route)
        {
            continue;
        }

        char nh[64], lp[16], med[16], as_path[64];
        bgp_nexthop_to_str(&route->nexthop, nh, sizeof(nh));
        bgp_route_fmt_fields(route, lp, sizeof(lp), med, sizeof(med), as_path, sizeof(as_path));

        g_string_append_printf(ctx->buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", BGP_RT_COL_NET, first ? prefix_str : "",
                               BGP_RT_COL_NH, nh, BGP_RT_COL_LP, lp, BGP_RT_COL_MED, med, BGP_RT_COL_ORIG,
                               bgp_origin_str(route->attr.origin), as_path);

        if (first)
        {
            ctx->listed_heads++;
            first = FALSE;
        }
        ctx->listed_routes++;
    }

    return FALSE;
}

/**
 * @brief 显示单条前缀的所有路径详情（供 show bgp route af ... <ip> <masklen> 使用）
 */
static void bgp_show_route_detail(GString *buf, const bgp_rthead_t *head)
{
    uint32_t path_count = g_hash_table_size(head->route_hash);
    g_string_append_printf(buf, "  Paths: %u\r\n\r\n", path_count);

    GHashTableIter iter;
    gpointer rkey, rval;
    g_hash_table_iter_init(&iter, (GHashTable *)head->route_hash);
    while (g_hash_table_iter_next(&iter, &rkey, &rval))
    {
        (void)rkey;
        const bgp_route_node_t *route = (const bgp_route_node_t *)rval;
        if (!route)
        {
            continue;
        }

        char peer[64], nh[64], lp[16], med[16], as_path[256], ts[32];
        net_addr_to_str(&route->source, peer, sizeof(peer));
        bgp_nexthop_to_str(&route->nexthop, nh, sizeof(nh));
        bgp_route_fmt_fields(route, lp, sizeof(lp), med, sizeof(med), as_path, sizeof(as_path));
        bgp_fmt_time_usec(route->updated_at_usec, ts, sizeof(ts));

        g_string_append_printf(buf, "  Peer       : %s\r\n", peer);
        g_string_append_printf(buf, "    NextHop  : %s\r\n", nh);
        g_string_append_printf(buf, "    LocPref  : %s\r\n", lp);
        g_string_append_printf(buf, "    MED      : %s\r\n", med);
        g_string_append_printf(buf, "    Origin   : %s\r\n", bgp_origin_str(route->attr.origin));
        g_string_append_printf(buf, "    AS-Path  : %s\r\n", as_path);

        if (route->attr.communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Community: %s\r\n", route->attr.communities);
        }
        if (route->attr.ext_communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Ext-Comm : %s\r\n", route->attr.ext_communities);
        }
        if (route->attr.large_communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Lrg-Comm : %s\r\n", route->attr.large_communities);
        }
        if (route->attr.aggregator[0] != '\0')
        {
            g_string_append_printf(buf, "    Aggregator: %s\r\n", route->attr.aggregator);
        }
        if (route->attr.has_originator_id)
        {
            char oid[64];
            net_addr_to_str(&route->attr.originator_id, oid, sizeof(oid));
            g_string_append_printf(buf, "    Originator: %s\r\n", oid);
        }
        g_string_append_printf(buf, "    Updated  : %s\r\n\r\n", ts);
    }
}

/**
 * @brief 处理 show bgp route af ipv4-unicast|ipv6-unicast [<ip> <masklen>] 命令
 *
 * group_id=10, cfg_id: 1=ipv4-unicast, 2=ipv6-unicast, 3=ip-address, 4=masklen
 * 不带 ip/masklen 时显示路由表（table），带时显示单前缀详情
 */
static int handle_bgp_show_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    gboolean has_af = FALSE;
    char ip_str[64] = {0};
    uint32_t masklen = 0;
    gboolean has_ip = FALSE;
    gboolean has_masklen = FALSE;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_UNICAST;
                has_af = TRUE;
                break;
            case 2:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_UNICAST;
                has_af = TRUE;
                break;
            case 3:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ip_str, sizeof(ip_str), "%s", s);
                    has_ip = TRUE;
                }
                break;
            }
            case 4:
                masklen = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_masklen = TRUE;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_af)
    {
        bgp_send_cli_response(msg,
                              "BGP Error: Missing address-family. Use 'af ipv4-unicast' or 'af ipv6-unicast'.\r\n");
        return ERRCODE_FAIL;
    }

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);
    if (!vrf)
    {
        bgp_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));

    GString *resp_buf = g_string_new("");
    if (!resp_buf)
    {
        bgp_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    /* ----------------------------------------------------------------
     * 路由详情模式：show bgp route af ... <ip> <masklen>
     * ---------------------------------------------------------------- */
    if (has_ip && has_masklen)
    {
        /* 构造 NLRI 查找键 */
        bgp_nlri_entry_t nlri;
        memset(&nlri, 0, sizeof(nlri));
        nlri.afi = ctx.afi;
        nlri.safi = ctx.safi;
        nlri.type = BGP_NLRI_PREFIX;
        nlri.prefix.prefix.prefix_len = (uint8_t)masklen;
        nlri.prefix.prefix.addr.family = (ctx.afi == BGP_AFI_IPV6) ? AF_INET6 : AF_INET;
        if (inet_pton(nlri.prefix.prefix.addr.family, ip_str, &nlri.prefix.prefix.addr.u) != 1)
        {
            g_string_free(resp_buf, TRUE);
            bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
            return ERRCODE_FAIL;
        }
        snprintf(nlri.key, sizeof(nlri.key), "%s/%u", ip_str, masklen);

        g_string_append_printf(resp_buf, "\r\nBGP Route Detail: %s/%u (AF: %s)\r\n", ip_str, masklen,
                               bgp_af_str(ctx.afi, ctx.safi));
        g_string_append(resp_buf, "============================================================\r\n");

        if (!inst || !inst->rib)
        {
            g_string_append(resp_buf, "  (no RIB)\r\n");
            return bgp_cli_send_chunked_response(msg, resp_buf);
        }

        const bgp_rthead_t *head = bgp_rib_lookup_head(inst->rib, &nlri);
        if (!head)
        {
            g_string_append_printf(resp_buf, "  Route %s/%u not found.\r\n", ip_str, masklen);
            return bgp_cli_send_chunked_response(msg, resp_buf);
        }

        bgp_show_route_detail(resp_buf, head);
        return bgp_cli_send_chunked_response(msg, resp_buf);
    }

    /* ----------------------------------------------------------------
     * 路由表模式：show bgp route af ...
     * ---------------------------------------------------------------- */
    g_string_append_printf(resp_buf, "\r\nBGP Routes (AF: %s)\r\n", bgp_af_str(ctx.afi, ctx.safi));
    g_string_append(resp_buf, "============================================================\r\n");

    if (!inst || !inst->rib || bgp_rib_route_count(inst->rib) == 0)
    {
        g_string_append(resp_buf, "  (no routes)\r\n\r\n");
        return bgp_cli_send_chunked_response(msg, resp_buf);
    }

    g_string_append_printf(resp_buf, "  Networks: %-6u  Paths: %u\r\n\r\n", bgp_rib_head_count(inst->rib),
                           bgp_rib_route_count(inst->rib));

    /* 表头 */
    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", BGP_RT_COL_NET, "Network", BGP_RT_COL_NH,
                           "NextHop", BGP_RT_COL_LP, "LocPref", BGP_RT_COL_MED, "MED", BGP_RT_COL_ORIG, "Origin",
                           "AS-Path");
    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", BGP_RT_COL_NET, "------------------------",
                           BGP_RT_COL_NH, "--------------------", BGP_RT_COL_LP, "--------", BGP_RT_COL_MED, "--------",
                           BGP_RT_COL_ORIG, "------------", "--------");

    bgp_show_route_ctx_t show_ctx;
    show_ctx.buf = resp_buf;
    show_ctx.listed_heads = 0;
    show_ctx.listed_routes = 0;

    g_tree_foreach(inst->rib->head_tree, bgp_show_route_head_cb, &show_ctx);

    g_string_append_printf(resp_buf, "\r\nTotal: %u networks, %u paths\r\n\r\n", show_ctx.listed_heads,
                           show_ctx.listed_routes);

    return bgp_cli_send_chunked_response(msg, resp_buf);
}

/**
 * @brief 处理 show bgp neighbor af ipv4-unicast|ipv6-unicast [<ip>] 命令
 *
 * group_id=9, cfg_id: 1=ipv4-unicast, 2=ipv6-unicast, 3=ip-address
 */
static int handle_bgp_show_neighbor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char ip_buf[64] = {0};
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    gboolean has_af = FALSE;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1: /* ipv4-unicast */
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_UNICAST;
                has_af = TRUE;
                break;
            case 2: /* ipv6-unicast */
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_UNICAST;
                has_af = TRUE;
                break;
            case 3: /* <ip-address> 参数 */
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", s);
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_af)
    {
        bgp_send_cli_response(msg,
                              "BGP Error: Missing address-family. Use 'af ipv4-unicast' or 'af ipv6-unicast'.\r\n");
        return ERRCODE_FAIL;
    }

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);
    if (!vrf)
    {
        bgp_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }

    /* show-all 模式：无 IP 参数时显示当前 AF 实例下所有邻居摘要 */
    if (ip_buf[0] == '\0')
    {
        bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));

        GString *resp_buf = g_string_new("");
        if (!resp_buf)
        {
            bgp_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
            return ERRCODE_FAIL;
        }

        g_string_append_printf(resp_buf, "\r\nBGP Neighbors (AF: %s)\r\n", bgp_af_str(ctx.afi, ctx.safi));
        g_string_append(resp_buf, "============================================================\r\n");

        if (!inst || g_hash_table_size(inst->peer_hash) == 0)
        {
            g_string_append(resp_buf, "  (no neighbors configured)\r\n");
        }
        else
        {
            g_string_append_printf(resp_buf, "  %-17s%-11s%-17s%s\r\n", "Neighbor", "Remote-AS", "Router-ID", "State");
            g_string_append_printf(resp_buf, "  %-17s%-11s%-17s%s\r\n", "---------------", "---------",
                                   "---------------", "-----------");

            GHashTableIter iter;
            gpointer key, val;
            g_hash_table_iter_init(&iter, inst->peer_hash);
            while (g_hash_table_iter_next(&iter, &key, &val))
            {
                bgp_peer_t *peer = (bgp_peer_t *)val;
                bgp_session_t *psess = bgp_vrf_find_session(vrf, &peer->addr);

                char nbr_ip[64];
                net_addr_to_str(&peer->addr, nbr_ip, sizeof(nbr_ip));

                char _psess_rid_str[16];
                if (psess && psess->remote_id)
                {
                    struct in_addr _tmp;
                    _tmp.s_addr = htonl(psess->remote_id);
                    inet_ntop(AF_INET, &_tmp, _psess_rid_str, sizeof(_psess_rid_str));
                }
                else
                {
                    snprintf(_psess_rid_str, sizeof(_psess_rid_str), "0.0.0.0");
                }
                const char *rid = _psess_rid_str;
                uint32_t ras = psess ? psess->remote_as : 0;
                const char *state = psess ? sess_state_str(psess) : "Idle";

                g_string_append_printf(resp_buf, "  %-17s%-11u%-17s%s\r\n", nbr_ip, ras, rid, state);
            }
        }

        g_string_append(resp_buf, "\r\n");
        return bgp_cli_send_chunked_response(msg, resp_buf);
    }

    net_addr_t ip_addr;
    if (net_addr_from_str(ip_buf, &ip_addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, &ip_addr);
    if (!sess)
    {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "BGP Error: Neighbor %s not found.\r\n", ip_buf);
        bgp_send_cli_response(msg, tmp);
        return ERRCODE_FAIL;
    }

    /* 查找 AF 实例，判断邻居是否在该 AF 下使能（peer_hash 以二进制 net_addr_t* 为键） */
    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));
    gboolean af_enabled = (inst && g_hash_table_lookup(inst->peer_hash, &ip_addr));

    /* 构建显示内容 */
    GString *resp_buf = g_string_new("");
    if (!resp_buf)
    {
        bgp_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    g_string_append_printf(resp_buf, "\r\nBGP Neighbor: %s\r\n", ip_buf);
    g_string_append(resp_buf, "==========================================\r\n");
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "Remote AS", sess->remote_as);
    char _sess_rid_str[32];
    if (sess->remote_id)
    {
        struct in_addr _tmp;
        _tmp.s_addr = htonl(sess->remote_id);
        inet_ntop(AF_INET, &_tmp, _sess_rid_str, sizeof(_sess_rid_str));
    }
    else
    {
        snprintf(_sess_rid_str, sizeof(_sess_rid_str), "(not established)");
    }
    g_string_append_printf(resp_buf, "  %-24s: %s\r\n", "Remote Router-ID", _sess_rid_str);
    g_string_append_printf(resp_buf, "  %-24s: %s\r\n", "Session State", sess_state_str(sess));

    /* 能力表格 */
    g_string_append(resp_buf, "\r\n  Capabilities:\r\n");
    g_string_append_printf(resp_buf, "  %-16s  %-10s  %-10s  %-10s\r\n", "Feature", "Local", "Remote", "Negotiated");
    g_string_append_printf(resp_buf, "  %-16s  %-10s  %-10s  %-10s\r\n", "---------------", "---------", "---------",
                           "---------");
    g_string_append_printf(resp_buf, "  %-16s  %-10s  %-10s  %-10s\r\n", "AS4", cap_yn(sess->flags, BGP_SESS_CAP_AS4),
                           cap_yn(sess->remote_caps, BGP_SESS_CAP_AS4),
                           cap_yn(sess->negotiated_caps, BGP_SESS_CAP_AS4));
    g_string_append_printf(resp_buf, "  %-16s  %-10s  %-10s  %-10s\r\n", "Route-Refresh",
                           cap_yn(sess->flags, BGP_SESS_CAP_ROUTE_REFRESH),
                           cap_yn(sess->remote_caps, BGP_SESS_CAP_ROUTE_REFRESH),
                           cap_yn(sess->negotiated_caps, BGP_SESS_CAP_ROUTE_REFRESH));

    /* Hold Time */
    g_string_append(resp_buf, "\r\n  Hold Time:\r\n");
    g_string_append_printf(resp_buf, "  %-24s: %u s\r\n", "Local (sent)", BGP_HOLD_TIME);
    if (sess->remote_hold)
    {
        g_string_append_printf(resp_buf, "  %-24s: %u s\r\n", "Remote (received)", sess->remote_hold);
    }
    else
    {
        g_string_append_printf(resp_buf, "  %-24s: %s\r\n", "Remote (received)", "(not established)");
    }
    g_string_append_printf(resp_buf, "  %-24s: %u s\r\n", "Negotiated", sess->negotiated_hold);

    /* 协商地址族 */
    g_string_append(resp_buf, "\r\n  Negotiated Address Families:\r\n");
    if (sess->negotiated_afs && sess->negotiated_afs->len > 0)
    {
        for (guint _af_i = 0; _af_i < sess->negotiated_afs->len; _af_i++)
        {
            guint32 packed = g_array_index(sess->negotiated_afs, guint32, _af_i);
            uint16_t _afi = (uint16_t)(packed >> 16);
            uint8_t _safi = (uint8_t)(packed & 0xFF);
            g_string_append_printf(resp_buf, "    afi=%u safi=%u\r\n", _afi, _safi);
        }
    }
    else
    {
        g_string_append(resp_buf, "    (none)\r\n");
    }

    /* AF 使能状态 */
    char af_label[64];
    snprintf(af_label, sizeof(af_label), "AF %s", bgp_af_str(ctx.afi, ctx.safi));
    g_string_append_printf(resp_buf, "\r\n  %-24s: %s\r\n", af_label, af_enabled ? "Enabled" : "Disabled");
    g_string_append(resp_buf, "\r\n");

    return bgp_cli_send_chunked_response(msg, resp_buf);
}

// ============================================================================
// 主入口
// ============================================================================

// ============================================================================
// Group 11: import-route 协议导入命令
//
// cfg-id 映射：1=no, 2=static
// ============================================================================

static int handle_bgp_import_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_cli_ctx_t bctx = bgp_cli_ctx_default();
    gboolean is_no = FALSE;
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
            case 1:
                is_no = TRUE;
                break;
            case 2:
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

    if (!g_bgp_local || !g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "Error: BGP protocol not configured\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, bctx.vrf_id);
    if (!vrf)
    {
        bgp_send_cli_response(msg, "Error: VRF does not exist\r\n");
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = bgp_vrf_get_or_create_instance(vrf, bctx.afi, bctx.safi);
    if (!inst)
    {
        bgp_send_cli_response(msg, "Error: Address family instance creation failed\r\n");
        return ERRCODE_FAIL;
    }

    /* 更新本地导入标志 */
    if (is_no)
    {
        inst->import_protos &= ~(1u << ROUTE_PROTOCOL_STATIC);
    }
    else
    {
        inst->import_protos |= (1u << ROUTE_PROTOCOL_STATIC);
    }

    /* 持久化到数据库 */
    bgp_db_set_import_protos(g_bgp_local->dev_ipc_ctx, bctx.vrf_id, bctx.afi, bctx.safi, inst->import_protos);

    /* 异步发送subscribe/unsubscribe请求到 ROUTE 模块（fire-and-forget，不等响应） */
    route_subscribe_req_t *req = (route_subscribe_req_t *)g_malloc(sizeof(route_subscribe_req_t));
    req->protocol = ROUTE_PROTOCOL_STATIC;
    req->vrf_id = ROUTE_VRF_DEFAULT;
    req->flags = is_no ? 0u : ROUTE_SUBSCRIBE_FLAG_FULL; /* 开启导入时请求一次全量快照，避免遗漏已存在静态路由 */

    uint32_t sub_type = is_no ? ROUTE_MSG_TYPE_UNSUBSCRIBE : ROUTE_MSG_TYPE_SUBSCRIBE;
    dev_ipc_message_t *sub_msg = dev_ipc_message_create(sub_type, DEV_MODULE_ID_BGP, DEV_MODULE_ID_ROUTE, 0, req,
                                                        sizeof(route_subscribe_req_t), g_free);
    if (sub_msg)
    {
        if (dev_ipc_send(g_bgp_local->dev_ipc_ctx, DEV_MODULE_ID_ROUTE, sub_msg) != 0)
        {
            LOG_WARN("BGP: Failed to send route %s request (ROUTE module may not be ready)",
                     is_no ? "unsubscribe" : "subscribe");
        }
        dev_ipc_message_free(sub_msg);
    }

    bgp_send_cli_response(msg, is_no ? "import-route static disabled\r\n" : "import-route static enabled\r\n");
    return ERRCODE_SUCCESS;
}

int bgp_cli_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_bgp_local->show_stream, g_bgp_local->dev_ipc_ctx, DEV_MODULE_ID_BGP, msg);
}

void bgp_cli_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_bgp_local->show_stream);
}

int bgp_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_chunk_stream_reset(&g_bgp_local->show_stream);

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("Payload parsing failed");
        bgp_send_cli_response(msg, "BGP Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("Received TLV payload (group_id=%u)", parser.group_id);

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
        case BGP_CLI_GROUP_ID_SHOW_NEIGHBOR:
            result = handle_bgp_show_neighbor(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_SHOW_ROUTE:
            result = handle_bgp_show_route(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_IMPORT_ROUTE:
            result = handle_bgp_import_route(msg, &parser);
            break;
        default:
            LOG_WARN("Unknown group_id: %u", parser.group_id);
            bgp_send_cli_response(msg, "BGP Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
