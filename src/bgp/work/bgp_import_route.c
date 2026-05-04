/**
 * @file   bgp_import_route.c
 * @brief  将 ROUTE 模块推送的路由条目导入到 BGP RIB
 *
 * 数据流：
 *   ROUTE 模块 → IPC → worker 线程
 *       → bgp_import_route_on_update / on_report
 *           → bgp_import_route_entries (批量循环)
 *               → bgp_import_route_entry (单条：校验/构造 NLRI/写 RIB/入 calc_queue)
 */
#include "bgp_import_route.h"

#include <glib.h>
#include <string.h>
#include <sys/socket.h>

#include "bgp.h"
#include "bgp_attr_intern.h"
#include "bgp_calc.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "tunnel.h"

#define BGP_IMPORT_LABEL_TIMEOUT_MS 3000u

static void bgp_import_fill_label_req(tunnel_label_req_t *req, uint32_t vrf_id, bgp_afi_t afi, uint8_t prefix_len,
                                      const net_addr_t *prefix_addr)
{
    memset(req, 0, sizeof(*req));
    req->vrf_id = vrf_id;
    req->afi = (uint16_t)afi;
    req->source_type = TUNNEL_SOURCE_BGP_LU;
    req->owner_module_id = DEV_MODULE_ID_BGP;
    req->owner_id = ((uint32_t)afi << 16) | (uint32_t)BGP_SAFI_LABELED;
    req->fec.vrf_id = vrf_id;
    req->fec.afi = (uint16_t)afi;
    req->fec.prefix_len = prefix_len;
    req->fec.addr = *prefix_addr;
}

static int bgp_import_prepare_labeled_nlri(bgp_nlri_entry_t *nlri, const route_msg_entry_t *entry)
{
    if (!nlri || !entry || !g_bgp_local || !g_bgp_local->dev_ipc_ctx)
    {
        return 0;
    }

    tunnel_label_req_t req;
    bgp_import_fill_label_req(&req, entry->vrf_id, (bgp_afi_t)entry->afi, entry->prefix_len, &entry->prefix_addr);

    uint32_t label = 0;
    if (tunnel_rpc_label_alloc(g_bgp_local->dev_ipc_ctx, &req, &label, BGP_IMPORT_LABEL_TIMEOUT_MS) != ERRCODE_SUCCESS)
    {
        char prefix[64];
        net_addr_to_str(&entry->prefix_addr, prefix, sizeof(prefix));
        LOG_WARN("BGP: failed to allocate LU label vrf=%u afi=%u prefix=%s/%u", entry->vrf_id, (unsigned)entry->afi,
                 prefix, entry->prefix_len);
        return 0;
    }

    nlri->safi = BGP_SAFI_LABELED;
    nlri->prefix.has_label = true;
    nlri->prefix.label = label;

    return 1;
}

static void bgp_import_release_labeled_label(const route_msg_entry_t *entry)
{
    if (!entry || !g_bgp_local || !g_bgp_local->dev_ipc_ctx)
    {
        return;
    }

    tunnel_label_req_t req;
    bgp_import_fill_label_req(&req, entry->vrf_id, (bgp_afi_t)entry->afi, entry->prefix_len, &entry->prefix_addr);
    (void)tunnel_rpc_label_release(g_bgp_local->dev_ipc_ctx, &req);
}

static void bgp_import_release_labeled_label_for_nlri(const bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !nlri || nlri->type != BGP_NLRI_PREFIX || nlri->safi != BGP_SAFI_LABELED || !g_bgp_local ||
        !g_bgp_local->dev_ipc_ctx)
    {
        return;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : ROUTE_VRF_DEFAULT;
    tunnel_label_req_t req;
    bgp_import_fill_label_req(&req, vrf_id, (bgp_afi_t)nlri->afi, nlri->prefix.prefix.prefix_len,
                              &nlri->prefix.prefix.addr);
    (void)tunnel_rpc_label_release(g_bgp_local->dev_ipc_ctx, &req);
}

typedef struct bgp_import_cleanup_item
{
    bgp_nlri_entry_t nlri;
    net_addr_t source;
} bgp_import_cleanup_item_t;

typedef struct bgp_import_cleanup_collect_ctx
{
    GPtrArray *items;
    uint32_t import_proto;
    gboolean all_import_protos;
} bgp_import_cleanup_collect_ctx_t;

static gboolean bgp_import_proto_matches(uint32_t route_proto, uint32_t import_proto, gboolean all_import_protos)
{
    if (all_import_protos)
    {
        return TRUE;
    }
    if (route_proto == import_proto)
    {
        return TRUE;
    }
    return import_proto == ROUTE_PROTOCOL_STATIC && route_proto == ROUTE_PROTOCOL_BLACKHOLE;
}

static gboolean bgp_import_collect_rib_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    bgp_import_cleanup_collect_ctx_t *ctx = (bgp_import_cleanup_collect_ctx_t *)user_data;
    const bgp_rthead_t *head = (const bgp_rthead_t *)value;
    if (!ctx || !ctx->items || !head)
    {
        return FALSE;
    }

    for (const GList *l = head->route_list; l; l = l->next)
    {
        const bgp_route_node_t *route = (const bgp_route_node_t *)l->data;
        if (!route || !BIT_TEST(route->flags, BGP_ROUTE_FLAG_IMPORT) || BIT_TEST(route->flags, BGP_ROUTE_FLAG_STALE))
        {
            continue;
        }
        if (!bgp_import_proto_matches(route->import_proto, ctx->import_proto, ctx->all_import_protos))
        {
            continue;
        }

        bgp_import_cleanup_item_t *item = g_malloc0(sizeof(*item));
        item->nlri = head->nlri;
        item->source = route->source;
        g_ptr_array_add(ctx->items, item);
    }
    return FALSE;
}

static void bgp_import_collect_inst_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib,
                                           gpointer user_data)
{
    (void)inst;
    (void)entry;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    g_tree_foreach(rib->head_tree, bgp_import_collect_rib_cb, user_data);
}

static uint32_t bgp_import_route_cleanup_instance_common(bgp_instance_t *inst, uint32_t import_proto,
                                                         gboolean all_import_protos)
{
    if (!inst)
    {
        return 0;
    }

    bgp_import_cleanup_collect_ctx_t ctx = {
        .items = g_ptr_array_new_with_free_func(g_free),
        .import_proto = import_proto,
        .all_import_protos = all_import_protos,
    };
    bgp_inst_foreach_rib(inst, bgp_import_collect_inst_rib_cb, &ctx);

    uint32_t removed = 0;
    for (guint i = 0; i < ctx.items->len; ++i)
    {
        const bgp_import_cleanup_item_t *item = (const bgp_import_cleanup_item_t *)g_ptr_array_index(ctx.items, i);
        bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, &item->nlri);
        if (!rib)
        {
            continue;
        }

        int rc = bgp_rib_unreach_one(rib, &item->nlri, &item->source);
        if (rc != 1)
        {
            continue;
        }

        removed++;
        if (inst->calc_queue)
        {
            bgp_calc_queue_push(inst->calc_queue, inst, &item->nlri);
        }
        if (item->nlri.safi == BGP_SAFI_LABELED)
        {
            bgp_import_release_labeled_label_for_nlri(inst, &item->nlri);
        }
    }

    if (removed > 0)
    {
        LOG_INFO("BGP: cleaned %u import-route entries afi=%u safi=%u proto=%s%u", removed, (unsigned)inst->afi,
                 (unsigned)inst->safi, all_import_protos ? "all/" : "", import_proto);
    }

    g_ptr_array_free(ctx.items, TRUE);
    return removed;
}

uint32_t bgp_import_route_cleanup_instance(bgp_instance_t *inst, uint32_t import_proto)
{
    return bgp_import_route_cleanup_instance_common(inst, import_proto, FALSE);
}

uint32_t bgp_import_route_cleanup_instance_all(bgp_instance_t *inst)
{
    return bgp_import_route_cleanup_instance_common(inst, 0, TRUE);
}

/**
 * @brief 将一条 ROUTE 路由条目导入（或撤销）到 BGP RIB
 * @return 1=已处理，0=被过滤/忽略
 */
static int bgp_import_route_entry_to_safi(const route_msg_entry_t *entry, bgp_vrf_t *vrf, bgp_afi_t afi,
                                          bgp_safi_t safi)
{
    if (!entry || !vrf)
    {
        return 0;
    }

    if ((afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6) || (safi != BGP_SAFI_UNICAST && safi != BGP_SAFI_LABELED))
    {
        return 0;
    }
    sa_family_t expected_family = (afi == BGP_AFI_IPV4) ? AF_INET : AF_INET6;
    uint8_t max_prefix_len = (afi == BGP_AFI_IPV4) ? 32u : 128u;
    if (entry->prefix_addr.family != expected_family || entry->source_addr.family != expected_family ||
        entry->nexthop_addr.family != expected_family || entry->prefix_len > max_prefix_len)
    {
        return 0;
    }

    bgp_instance_t *inst = (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi));
    if (!inst)
    {
        return 0;
    }

    /* ROUTE_PROTOCOL_BLACKHOLE（null0 黑洞路由）在用户视角下也是静态路由，
     * 因此在 import-route static 开关打开时一并允许导入。 */
    uint32_t effective_proto = entry->protocol;
    if (effective_proto == ROUTE_PROTOCOL_BLACKHOLE)
    {
        effective_proto = ROUTE_PROTOCOL_STATIC;
    }
    if (!(inst->import_protos & (1u << effective_proto)))
    {
        /* import-route 路径遵循 import-route 协议开关 */
        return 0;
    }

    /* 构建 NLRI entry（直接使用二进制地址，无字符串转换） */
    bgp_nlri_entry_t nlri;
    memset(&nlri, 0, sizeof(nlri));
    nlri.afi = (uint16_t)afi;
    nlri.safi = (uint8_t)safi;
    nlri.type = BGP_NLRI_PREFIX;
    nlri.prefix.prefix.prefix_len = entry->prefix_len;
    nlri.prefix.prefix.addr = entry->prefix_addr;
    nlri.prefix.has_rd = false;
    nlri.prefix.has_label = false;

    if (safi == BGP_SAFI_LABELED && !bgp_import_prepare_labeled_nlri(&nlri, entry))
    {
        return 0;
    }

    net_addr_t src = entry->source_addr;

    bgp_rib_t *rib = bgp_inst_rib_ensure_for_nlri(inst, &nlri);
    if (!rib)
    {
        return 0;
    }

    if (entry->is_withdraw)
    {
        int rc = bgp_rib_unreach_one(rib, &nlri, &src);
        /* 与对端 UPDATE 处理保持一致：撤销成功后触发一次优选，决定是否发 WITHDRAW */
        if (rc == 1 && inst->calc_queue)
        {
            bgp_calc_queue_push(inst->calc_queue, inst, &nlri);
        }
        if (safi == BGP_SAFI_LABELED)
        {
            bgp_import_release_labeled_label(entry);
        }
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(&nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: Import route withdraw %s", nlri_str);
    }
    else
    {
        bgp_attr_t attr;
        bgp_attr_build_imported(&attr);

        bgp_nexthop_t nexthop;
        bgp_nexthop_from_addr(&nexthop, &entry->nexthop_addr);

        bgp_rthead_t *head = bgp_rib_ensure_head(rib, &nlri);
        if (!head)
        {
            return 0;
        }

        bgp_route_node_t *route = bgp_rthead_lookup_route_mut(head, &src);
        int rc = 0;
        if (!route)
        {
            route = bgp_rthead_create_route(rib, head, &src);
            if (!route)
            {
                return 0;
            }
            rc = 1;
        }
        if (bgp_rib_route_apply_reach(route, (uint32_t)entry->protocol, &attr, &nexthop) != 0)
        {
            return 0;
        }
        bgp_route_set_label_from_nlri(route, &nlri, BGP_ROUTE_LABEL_SOURCE_LOCAL);
        if (rc >= 0 && inst->calc_queue)
        {
            bgp_calc_queue_push(inst->calc_queue, inst, &nlri);
        }

        char nh_str[64];
        char src_str[64];
        net_addr_to_str(&entry->nexthop_addr, nh_str, sizeof(nh_str));
        net_addr_to_str(&entry->source_addr, src_str, sizeof(src_str));
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(&nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: Import route add %s nh=%s src=%s", nlri_str, nh_str, src_str);
    }

    return 1;
}

static int bgp_import_route_entry(const route_msg_entry_t *entry)
{
    if (!entry)
    {
        return 0;
    }

    if (!g_bgp_work_local || !g_bgp_work_local->protocol)
    {
        return 0;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_work_local->protocol, entry->vrf_id);
    if (!vrf)
    {
        return 0;
    }

    bgp_afi_t afi = (bgp_afi_t)entry->afi;
    if (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6)
    {
        return 0;
    }

    if (entry->safi != 0 && entry->safi != BGP_SAFI_UNICAST)
    {
        return 0;
    }

    int handled = 0;
    handled += bgp_import_route_entry_to_safi(entry, vrf, afi, BGP_SAFI_UNICAST);
    handled += bgp_import_route_entry_to_safi(entry, vrf, afi, BGP_SAFI_LABELED);
    return handled;
}

/**
 * @brief 批量导入 ROUTE 路由条目
 * @return 实际导入条目数
 */
static uint32_t bgp_import_route_entries(const route_msg_entry_t *entries, uint32_t route_count)
{
    if (!entries || route_count == 0)
    {
        return 0;
    }

    uint32_t imported = 0;
    for (uint32_t i = 0; i < route_count; ++i)
    {
        imported += (uint32_t)bgp_import_route_entry(&entries[i]);
    }
    return imported;
}

uint32_t bgp_import_route_on_update(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(route_msg_entry_t))
    {
        LOG_WARN("BGP: ROUTE_UPDATE payload too short: %u bytes", msg ? msg->payload_len : 0u);
        return 0;
    }

    const route_msg_entry_t *entry = (const route_msg_entry_t *)msg->payload;
    return bgp_import_route_entries(entry, 1);
}

uint32_t bgp_import_route_on_report(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(route_msg_report_t))
    {
        LOG_WARN("BGP: ROUTE_REPORT payload too short: %u bytes", msg ? msg->payload_len : 0u);
        return 0;
    }

    const route_msg_report_t *report = (const route_msg_report_t *)msg->payload;
    size_t routes_bytes = (size_t)msg->payload_len - sizeof(route_msg_report_t);
    uint32_t max_routes = (uint32_t)(routes_bytes / sizeof(route_msg_entry_t));
    if (report->route_count > max_routes)
    {
        LOG_WARN("BGP: ROUTE_REPORT malformed: payload=%u route_count=%u max=%u", msg->payload_len, report->route_count,
                 max_routes);
        return 0;
    }

    return bgp_import_route_entries(report->routes, report->route_count);
}
