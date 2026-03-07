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

#include "bgp_db.h"
#include "bgp_main.h"
#include "bgp_protocol.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void bgp_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_bgp_local->dev_ipc_ctx, resp);
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

/**
 * BGP 上下文变量 ID（独立命名空间，与 XML cfg-id 无关）
 * 与 commands.xml <context-out ctx-id="N"> 中的 N 对应
 */
#define BGP_CTX_VAR_AS 1   /**< BGP AS 号（BGP 视图及 AF 视图均携带） */
#define BGP_CTX_VAR_VRF 2  /**< VRF ID */
#define BGP_CTX_VAR_AFI 3  /**< 地址族（bgp_afi_t） */
#define BGP_CTX_VAR_SAFI 4 /**< 子地址族（bgp_safi_t） */

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
        case BGP_CTX_VAR_VRF:
            ctx->vrf_id = (uint32_t)cli_tlv_entry_get_int(entry);
            break;
        case BGP_CTX_VAR_AFI:
            ctx->afi = (bgp_afi_t)cli_tlv_entry_get_int(entry);
            break;
        case BGP_CTX_VAR_SAFI:
            ctx->safi = (bgp_safi_t)cli_tlv_entry_get_int(entry);
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
        if (CFG_TLV_IS_VIEW_TEMPLATE(entry.cfg_id) || CLI_TLV_IS_CTX(&entry))
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

    /* 删除场景 */
    if (is_no)
    {
        if (g_bgp_local->protocol == NULL)
        {
            bgp_send_cli_response(msg, "");
            return ERRCODE_FAIL;
        }

        bgp_listen_stop();
        bgp_protocol_destroy(g_bgp_local->protocol);
        g_bgp_local->protocol = NULL;

        (void)bgp_db_del_as(g_bgp_local->dev_ipc_ctx);

        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (g_bgp_local->protocol == NULL)
    {
        /* 首次配置：建表并写入默认值 */
        if (bgp_db_init(g_bgp_local->dev_ipc_ctx) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database initialization failed.\r\n");
            return ERRCODE_FAIL;
        }
        bgp_db_ensure_defaults(g_bgp_local->dev_ipc_ctx);

        if (bgp_db_set_as(g_bgp_local->dev_ipc_ctx, as_number) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
        g_bgp_local->protocol = bgp_protocol_create(as_number);
        bgp_listen_start();
    }
    else
    {
        if (g_bgp_local->protocol->as_number != as_number)
        {
            bgp_send_cli_response(msg, "BGP Error: AS number mismatch.\r\n");
            return ERRCODE_FAIL;
        }
    }

    /* 框架根据 XML <context-out> 自动切换视图并写入上下文，模块只需返回空 OK */
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

    bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);

    if (is_no)
    {
        if (ip_buf[0] == '\0')
        {
            bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
            return ERRCODE_FAIL;
        }

        net_addr_t ip_addr;
        if (net_addr_from_str(ip_buf, &ip_addr) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
            return ERRCODE_FAIL;
        }

        if (vrf0)
        {
            bgp_session_t *del_sess = bgp_vrf_find_session(vrf0, &ip_addr);
            if (del_sess)
            {
                bgp_vrf_af_disable_neighbor(vrf0, BGP_AFI_IPV4, BGP_SAFI_UNICAST, &ip_addr);
                bgp_server_stop_session_conns(del_sess);
            }
            bgp_vrf_del_session(vrf0, &ip_addr);
        }

        int rows = bgp_db_del_session(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ip_buf);
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s deleted (%d row).\r\n", ip_buf, rows > 0 ? rows : 0);
        bgp_send_cli_response(msg, resp_buf);
        return ERRCODE_SUCCESS;
    }

    /* neighbor <ip> as <as-num> — 创建/更新 session */
    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!has_remote_as)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing remote AS number.\r\n");
        return ERRCODE_FAIL;
    }

    net_addr_t ip_addr;
    if (net_addr_from_str(ip_buf, &ip_addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_db_set_session(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ip_buf, remote_as) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    if (vrf0)
    {
        bgp_session_t *existing = bgp_vrf_find_session(vrf0, &ip_addr);
        if (existing)
        {
            existing->remote_as = remote_as;
        }
        else
        {
            bgp_session_t *sess = bgp_session_create(&ip_addr, remote_as, vrf0);
            bgp_vrf_add_session(vrf0, sess);
            LOG_INFO("BGP: neighbor %s 已配置，等待地址族使能后再启动监听", ip_buf);
        }
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
        if (CFG_TLV_IS_VIEW_TEMPLATE(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
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
    const char *af_name = bgp_af_str(ctx.afi, ctx.safi);

    /* 删除场景：no af ipv4-unicast */
    if (is_no)
    {
        if (vrf)
        {
            /* 收集该 AF 实例下的邻居地址，逐个停用，避免遍历中修改哈希表 */
            bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));
            if (inst && inst->peer_hash)
            {
                GList *addr_strs = NULL;
                GHashTableIter iter;
                gpointer k, v;
                g_hash_table_iter_init(&iter, inst->peer_hash);
                while (g_hash_table_iter_next(&iter, &k, &v))
                {
                    addr_strs = g_list_append(addr_strs, g_strdup((const char *)k));
                }
                for (GList *l = addr_strs; l; l = l->next)
                {
                    net_addr_t addr;
                    if (net_addr_from_str((const char *)l->data, &addr) == 0)
                    {
                        bgp_vrf_af_disable_neighbor(vrf, ctx.afi, ctx.safi, &addr);
                        bgp_session_t *sess = bgp_vrf_find_session(vrf, &addr);
                        if (sess && !bgp_vrf_neighbor_has_any_af(vrf, &addr))
                        {
                            bgp_server_stop_session_conns(sess);
                        }
                    }
                }
                g_list_free_full(addr_strs, g_free);
            }
            bgp_vrf_del_instance(vrf, ctx.afi, ctx.safi);
        }

        bgp_db_del_neighbors_by_afi(g_bgp_local->dev_ipc_ctx, af_name);
        bgp_db_del_instance(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ctx.afi, ctx.safi);

        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* 配置场景：af ipv4-unicast — 持久化实例并切换视图 */
    if (bgp_db_set_instance(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ctx.afi, ctx.safi) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    if (vrf)
    {
        bgp_vrf_get_or_create_instance(vrf, ctx.afi, ctx.safi);
    }

    /* 框架根据 XML <context-out> 自动切换视图并写入上下文，模块只需返回空 OK */
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

    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    net_addr_t ip_addr;
    if (net_addr_from_str(ip_buf, &ip_addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, ctx.vrf_id);
    const char *af_name = bgp_af_str(ctx.afi, ctx.safi);

    if (is_no)
    {
        bgp_session_t *sess = vrf ? bgp_vrf_find_session(vrf, &ip_addr) : NULL;

        if (vrf)
        {
            bgp_vrf_af_disable_neighbor(vrf, ctx.afi, ctx.safi, &ip_addr);
        }

        if (sess && vrf && !bgp_vrf_neighbor_has_any_af(vrf, &ip_addr))
        {
            bgp_server_stop_session_conns(sess);
        }

        int rows = bgp_db_del_neighbor(g_bgp_local->dev_ipc_ctx, ip_buf, af_name);
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s disabled for %s (%d row).\r\n", ip_buf, af_name,
                 rows > 0 ? rows : 0);
        bgp_send_cli_response(msg, resp_buf);
        return ERRCODE_SUCCESS;
    }

    /* neighbor <ip> enable — 使能地址族邻居 */
    if (!vrf || !bgp_vrf_find_session(vrf, &ip_addr))
    {
        bgp_send_cli_response(msg,
                              "BGP Error: Neighbor session not configured. Run 'neighbor <ip> as <as>' first.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_db_set_neighbor(g_bgp_local->dev_ipc_ctx, ip_buf, af_name) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    /* 首个 AF 使能时需启动主动连接，先检查使能前的状态 */
    gboolean first_af = !bgp_vrf_neighbor_has_any_af(vrf, &ip_addr);

    if (bgp_vrf_af_enable_neighbor(vrf, ctx.afi, ctx.safi, &ip_addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to enable AF neighbor.\r\n");
        return ERRCODE_FAIL;
    }

    /* 若是该 session 的第一个 AF，启动主动 TCP 连接 */
    if (first_af)
    {
        bgp_session_t *sess = bgp_vrf_find_session(vrf, &ip_addr);
        if (sess)
        {
            bgp_server_start_active_conn(sess);
        }
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
        if (CFG_TLV_IS_VIEW_TEMPLATE(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
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

    /* 删除场景：重置为默认值 */
    if (is_no)
    {
        if (vrf)
        {
            vrf->keepalive = BGP_TIMER_DEFAULT_KEEPALIVE;
            vrf->hold_time = BGP_TIMER_DEFAULT_HOLD;
        }
        (void)bgp_db_del_vrf_timers(g_bgp_local->dev_ipc_ctx, ctx.vrf_id);
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* 配置场景：参数校验 */
    if (keepalive == 0 || hold_time == 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing timer parameters.\r\n");
        return ERRCODE_FAIL;
    }
    if (hold_time <= keepalive)
    {
        bgp_send_cli_response(msg, "BGP Error: Hold time must be greater than keepalive time.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_db_set_vrf_timers(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, (uint16_t)keepalive, (uint16_t)hold_time) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    if (vrf)
    {
        vrf->keepalive = (uint16_t)keepalive;
        vrf->hold_time = (uint16_t)hold_time;
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
        if (CFG_TLV_IS_VIEW_TEMPLATE(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
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

    /* 删除场景 */
    if (is_no)
    {
        if (vrf)
        {
            vrf->router_id[0] = '\0';
        }
        (void)bgp_db_del_vrf_router_id(g_bgp_local->dev_ipc_ctx, ctx.vrf_id);
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* 配置场景 */
    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing router-id IP address.\r\n");
        return ERRCODE_FAIL;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip_buf, &addr) != 1)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IPv4 address.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_db_set_vrf_router_id(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, ip_buf) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    if (vrf)
    {
        snprintf(vrf->router_id, sizeof(vrf->router_id), "%s", ip_buf);
    }

    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 show bgp peer 命令
 *
 * group_id=2，直接构建格式化文本返回
 */
static int handle_bgp_peer_show(dev_ipc_message_t *msg)
{
    db_result_t *result = NULL;
    int ret = db_rpc_query(g_bgp_local->dev_ipc_ctx, BGP_TABLE_PROTOCOL, NULL, 0, NULL, &result);

    if (ret != 0 || !result)
    {
        bgp_send_cli_response(msg, "BGP Error: Database query failed.\r\n");
        return ERRCODE_FAIL;
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    size_t offset = 0;

    if (result->num_rows == 0)
    {
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "No BGP configuration found.\r\n");
    }
    else
    {
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\nBGP Information:\r\n");
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "============================\r\n");

        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            db_row_t *row = result->rows[i];
            for (uint32_t j = 0; j < row->num_fields; j++)
            {
                char value_str[256] = {0};
                switch (row->values[j].type)
                {
                    case DB_TYPE_INTEGER:
                        snprintf(value_str, sizeof(value_str), "%ld", row->values[j].data.i64);
                        break;
                    case DB_TYPE_REAL:
                        snprintf(value_str, sizeof(value_str), "%.6g", row->values[j].data.real);
                        break;
                    case DB_TYPE_TEXT:
                        if (row->values[j].data.text)
                        {
                            snprintf(value_str, sizeof(value_str), "%s", row->values[j].data.text);
                        }
                        break;
                    default:
                        snprintf(value_str, sizeof(value_str), "NULL");
                        break;
                }
                CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  %-20s: %s\r\n", row->field_names[j], value_str);
            }
        }
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\n");
    }

    db_result_free(result);

    /* 查询并显示 session 信息 */
    db_result_t *session_result = NULL;
    ret = db_rpc_query(g_bgp_local->dev_ipc_ctx, BGP_TABLE_SESSION, NULL, 0, NULL, &session_result);
    if (ret == 0 && session_result && session_result->num_rows > 0)
    {
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "BGP Sessions:\r\n");
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "----------------------------\r\n");
        for (uint32_t i = 0; i < session_result->num_rows; i++)
        {
            db_row_t *row = session_result->rows[i];
            const char *ip = db_row_get_text(row, "neighbor_ip", "N/A");
            int64_t as_num = db_row_get_int(row, "remote_as", 0);
            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  Neighbor: %-15s  AS: %ld\r\n", ip, as_num);
        }
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\n");
    }
    if (session_result)
    {
        db_result_free(session_result);
    }

    /* 查询并显示 neighbor 信息 */
    db_result_t *neighbor_result = NULL;
    ret = db_rpc_query(g_bgp_local->dev_ipc_ctx, BGP_TABLE_NEIGHBOR, NULL, 0, NULL, &neighbor_result);
    if (ret == 0 && neighbor_result && neighbor_result->num_rows > 0)
    {
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "BGP Address-Family Neighbors:\r\n");
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "----------------------------\r\n");
        for (uint32_t i = 0; i < neighbor_result->num_rows; i++)
        {
            db_row_t *row = neighbor_result->rows[i];
            const char *ip = db_row_get_text(row, "neighbor_ip", "N/A");
            const char *afi = db_row_get_text(row, "afi", "N/A");
            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  Neighbor: %-15s  AFI: %s\r\n", ip, afi);
        }
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\n");
    }
    if (neighbor_result)
    {
        db_result_free(neighbor_result);
    }

    bgp_send_cli_response(msg, resp_buf);
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
        if (CFG_TLV_IS_VIEW_TEMPLATE(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
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

    /* 删除场景：重置为默认值 */
    if (is_no)
    {
        if (vrf)
        {
            vrf->connect_retry = BGP_TIMER_DEFAULT_CONNECT_RETRY;
        }
        (void)bgp_db_del_vrf_connect_retry(g_bgp_local->dev_ipc_ctx, ctx.vrf_id);
        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* 配置场景：参数校验 */
    if (connect_retry == 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing connect-retry parameter.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_db_set_vrf_connect_retry(g_bgp_local->dev_ipc_ctx, ctx.vrf_id, (uint16_t)connect_retry) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    if (vrf)
    {
        vrf->connect_retry = (uint16_t)connect_retry;
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
        if (CFG_TLV_IS_VIEW_TEMPLATE(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
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
    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
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

    /* 更新内存标记位 */
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

    if (is_no)
    {
        BIT_CLR(sess->flags, cap_bit);
    }
    else
    {
        BIT_SET(sess->flags, cap_bit);
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
// 主入口
// ============================================================================

int bgp_cli_handle_continue(dev_ipc_message_t *msg)
{
    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

int bgp_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        bgp_send_cli_response(msg, "BGP Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case BGP_CLI_GROUP_ID_PROTOCOL:
            result = handle_bgp_protocol(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_SHOW:
            result = handle_bgp_peer_show(msg);
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
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            bgp_send_cli_response(msg, "BGP Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
