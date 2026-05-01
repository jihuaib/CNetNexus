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
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

/**
 * @brief 将一条 ROUTE 路由条目导入（或撤销）到 BGP RIB
 * @return 1=已处理，0=被过滤/忽略
 */
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

    /* 查找对应 AFI/SAFI 实例（不自动创建：未配置 AF 则忽略） */
    bgp_afi_t afi = (bgp_afi_t)entry->afi;
    bgp_safi_t safi = (entry->safi == 0) ? BGP_SAFI_UNICAST : (bgp_safi_t)entry->safi;
    if ((afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6) || safi != BGP_SAFI_UNICAST)
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
