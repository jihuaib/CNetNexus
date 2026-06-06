/**
 * @file   route_cfg_apply.c
 * @brief  Route 配置内存态应用实现（仅在 worker 线程调用）
 * @author jhb
 * @date   2026/03/28
 */
#include "route_cfg_apply.h"

#include <arpa/inet.h>
#include <string.h>

#include "db.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_calc.h"
#include "route_main.h"
#include "route_static.h"
#include "route_worker.h"

// ============================================================================
// 内部辅助函数
// ============================================================================

/**
 * @brief IPv6 地址字节数组按前缀步进递增（进位传播）
 */
static void ipv6_next_prefix(uint8_t *addr16, uint8_t prefix_len)
{
    int byte_idx = (prefix_len - 1) / 8;
    int bit_shift = 7 - ((prefix_len - 1) % 8);
    uint16_t carry = (uint16_t)(1 << bit_shift);

    for (int i = byte_idx; i >= 0 && carry; i--)
    {
        uint16_t sum = addr16[i] + carry;
        addr16[i] = (uint8_t)(sum & 0xFF);
        carry = sum >> 8;
    }
}

static uint64_t ipv4_to_u64(const net_addr_t *addr)
{
    return (uint64_t)ntohl(addr->u.v4.s_addr);
}

static unsigned __int128 ipv6_to_u128(const net_addr_t *addr)
{
    unsigned __int128 v = 0;
    for (int i = 0; i < 16; i++)
    {
        v = (v << 8) | addr->u.v6.s6_addr[i];
    }
    return v;
}

static int batch_ipv4_range(const net_addr_t *start_addr, uint8_t prefix_len, int64_t count, uint64_t *range_start,
                            uint64_t *range_end)
{
    if (!start_addr || !range_start || !range_end || start_addr->family != AF_INET || prefix_len < 1 ||
        prefix_len > 32 || count <= 0)
    {
        return -1;
    }

    uint64_t step = (prefix_len < 32) ? (1ULL << (32 - prefix_len)) : 1ULL;
    uint64_t first = ipv4_to_u64(start_addr);
    if ((uint64_t)(count - 1) > (((uint64_t)UINT32_MAX - first) / step))
    {
        return -1;
    }

    uint64_t last_start = first + ((uint64_t)(count - 1) * step);
    *range_start = first;
    *range_end = last_start + step - 1;
    return 0;
}

static int batch_ipv6_range(const net_addr_t *start_addr, uint8_t prefix_len, int64_t count,
                            unsigned __int128 *range_start, unsigned __int128 *range_end)
{
    if (!start_addr || !range_start || !range_end || start_addr->family != AF_INET6 || prefix_len < 1 ||
        prefix_len > 128 || count <= 0)
    {
        return -1;
    }

    unsigned __int128 step = ((unsigned __int128)1) << (128 - prefix_len);
    unsigned __int128 first = ipv6_to_u128(start_addr);
    unsigned __int128 max = ~((unsigned __int128)0);
    unsigned __int128 n = (unsigned __int128)count;
    if ((n - 1) > ((max - first) / step))
    {
        return -1;
    }

    unsigned __int128 last_start = first + ((n - 1) * step);
    *range_start = first;
    *range_end = last_start + step - 1;
    return 0;
}

static gboolean range_overlap_u64(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end)
{
    return (a_start <= b_end && b_start <= a_end) ? TRUE : FALSE;
}

static gboolean range_overlap_u128(unsigned __int128 a_start, unsigned __int128 a_end, unsigned __int128 b_start,
                                   unsigned __int128 b_end)
{
    return (a_start <= b_end && b_start <= a_end) ? TRUE : FALSE;
}

static int batch_check_conflict(const char *name, uint32_t vrf_id, uint16_t afi, const net_addr_t *start_addr,
                                uint8_t prefix_len, int64_t count, char *conflict_name, size_t conflict_name_len)
{
    if (conflict_name && conflict_name_len > 0)
    {
        conflict_name[0] = '\0';
    }

    if (afi == ROUTE_AFI_IPV4)
    {
        uint64_t new_start = 0;
        uint64_t new_end = 0;
        if (batch_ipv4_range(start_addr, prefix_len, count, &new_start, &new_end) != 0)
        {
            return -1;
        }

        for (GList *l = g_route_work_local->batch_entries; l; l = l->next)
        {
            route_batch_entry_t *be = (route_batch_entry_t *)l->data;
            if (!be || strcmp(be->name, name) == 0 || be->vrf_id != vrf_id || be->afi != afi)
            {
                continue;
            }

            uint64_t old_start = 0;
            uint64_t old_end = 0;
            if (batch_ipv4_range(&be->prefix_addr, be->prefix_len, 1, &old_start, &old_end) != 0)
            {
                continue;
            }
            if (range_overlap_u64(new_start, new_end, old_start, old_end))
            {
                if (conflict_name && conflict_name_len > 0)
                {
                    g_strlcpy(conflict_name, be->name, conflict_name_len);
                }
                return 1;
            }
        }
        return 0;
    }

    if (afi == ROUTE_AFI_IPV6)
    {
        unsigned __int128 new_start = 0;
        unsigned __int128 new_end = 0;
        if (batch_ipv6_range(start_addr, prefix_len, count, &new_start, &new_end) != 0)
        {
            return -1;
        }

        for (GList *l = g_route_work_local->batch_entries; l; l = l->next)
        {
            route_batch_entry_t *be = (route_batch_entry_t *)l->data;
            if (!be || strcmp(be->name, name) == 0 || be->vrf_id != vrf_id || be->afi != afi)
            {
                continue;
            }

            unsigned __int128 old_start = 0;
            unsigned __int128 old_end = 0;
            if (batch_ipv6_range(&be->prefix_addr, be->prefix_len, 1, &old_start, &old_end) != 0)
            {
                continue;
            }
            if (range_overlap_u128(new_start, new_end, old_start, old_end))
            {
                if (conflict_name && conflict_name_len > 0)
                {
                    g_strlcpy(conflict_name, be->name, conflict_name_len);
                }
                return 1;
            }
        }
        return 0;
    }

    return -1;
}

/**
 * @brief 生成 batch 路由并写入静态候选表和 batch_entries 列表
 *
 * @return 成功添加数量；-1 参数非法
 */
static int batch_do_add(const char *name, uint32_t vrf_id, uint16_t afi, const net_addr_t *start_addr,
                        uint8_t prefix_len, int64_t count, const net_addr_t *nexthop_addr, int32_t metric,
                        int32_t preference, const char *out_ifname)
{
    int added = 0;
    const char *safe_ifname = out_ifname ? out_ifname : "";

    if (!name || !start_addr || !nexthop_addr || count <= 0)
    {
        return -1;
    }

    if (afi == ROUTE_AFI_IPV4)
    {
        uint64_t range_start = 0;
        uint64_t range_end = 0;
        if (batch_ipv4_range(start_addr, prefix_len, count, &range_start, &range_end) != 0)
        {
            return -1;
        }

        uint64_t step = (prefix_len < 32) ? (1ULL << (32 - prefix_len)) : 1ULL;
        uint64_t addr = range_start;

        for (int64_t i = 0; i < count; i++)
        {
            struct in_addr cur;
            cur.s_addr = htonl((uint32_t)addr);

            net_addr_t prefix_addr;
            prefix_addr.family = AF_INET;
            prefix_addr.u.v4 = cur;

            route_static_add(vrf_id, ROUTE_AFI_IPV4, &prefix_addr, prefix_len, nexthop_addr, metric, preference,
                             safe_ifname);

            route_batch_entry_t *be = (route_batch_entry_t *)g_malloc0(sizeof(route_batch_entry_t));
            g_strlcpy(be->name, name, sizeof(be->name));
            be->vrf_id = vrf_id;
            be->afi = ROUTE_AFI_IPV4;
            be->prefix_len = prefix_len;
            be->metric = metric;
            be->preference = preference;
            be->prefix_addr = prefix_addr;
            be->nexthop_addr = *nexthop_addr;
            g_strlcpy(be->out_ifname, safe_ifname, sizeof(be->out_ifname));
            g_route_work_local->batch_entries = g_list_append(g_route_work_local->batch_entries, be);

            addr += step;
            added++;
        }
    }
    else if (afi == ROUTE_AFI_IPV6)
    {
        uint8_t addr16[16];
        unsigned __int128 range_start = 0;
        unsigned __int128 range_end = 0;
        if (batch_ipv6_range(start_addr, prefix_len, count, &range_start, &range_end) != 0)
        {
            return -1;
        }
        (void)range_start;
        (void)range_end;
        memcpy(addr16, start_addr->u.v6.s6_addr, sizeof(addr16));

        for (int64_t i = 0; i < count; i++)
        {
            struct in6_addr cur;
            memcpy(cur.s6_addr, addr16, 16);

            net_addr_t prefix_addr;
            prefix_addr.family = AF_INET6;
            prefix_addr.u.v6 = cur;

            route_static_add(vrf_id, ROUTE_AFI_IPV6, &prefix_addr, prefix_len, nexthop_addr, metric, preference,
                             safe_ifname);

            route_batch_entry_t *be = (route_batch_entry_t *)g_malloc0(sizeof(route_batch_entry_t));
            g_strlcpy(be->name, name, sizeof(be->name));
            be->vrf_id = vrf_id;
            be->afi = ROUTE_AFI_IPV6;
            be->prefix_len = prefix_len;
            be->metric = metric;
            be->preference = preference;
            be->prefix_addr = prefix_addr;
            be->nexthop_addr = *nexthop_addr;
            g_strlcpy(be->out_ifname, safe_ifname, sizeof(be->out_ifname));
            g_route_work_local->batch_entries = g_list_append(g_route_work_local->batch_entries, be);

            ipv6_next_prefix(addr16, prefix_len);
            added++;
        }
    }

    return added;
}

// ============================================================================
// 静态路由应用
// ============================================================================

void route_cfg_apply_static(route_apply_cmd_t *apply)
{
    if (!apply)
    {
        return;
    }

    switch (apply->op)
    {
        case ROUTE_APPLY_STATIC_ADD:
        {
            int ret = route_static_add(apply->u.static_add.vrf_id, apply->u.static_add.afi,
                                       &apply->u.static_add.prefix_addr, apply->u.static_add.prefix_len,
                                       &apply->u.static_add.nexthop_addr, apply->u.static_add.metric,
                                       apply->u.static_add.preference, apply->u.static_add.out_ifname);
            route_recompute_iter_paths();
            apply->rc = (ret >= 0) ? 1 : -1;
            break;
        }

        case ROUTE_APPLY_STATIC_DEL:
        {
            int ret = route_static_del(apply->u.static_del.vrf_id, apply->u.static_del.afi,
                                       &apply->u.static_del.prefix_addr, apply->u.static_del.prefix_len,
                                       &apply->u.static_del.nexthop_addr, apply->u.static_del.out_ifname);
            route_recompute_iter_paths();
            apply->rc = (ret > 0) ? ret : 0;
            break;
        }

        case ROUTE_APPLY_STATIC_DEL_PREFIX:
        {
            int ret =
                route_static_del_prefix(apply->u.static_del_prefix.vrf_id, apply->u.static_del_prefix.afi,
                                        &apply->u.static_del_prefix.prefix_addr, apply->u.static_del_prefix.prefix_len);
            route_recompute_iter_paths();
            apply->rc = (ret > 0) ? ret : 0;
            break;
        }

        case ROUTE_APPLY_STATIC_DEL_VRF:
        {
            int ret = route_static_del_vrf(apply->u.static_del_vrf.vrf_id);
            route_recompute_iter_paths();
            apply->rc = (ret > 0) ? ret : 0;
            break;
        }

        default:
            LOG_WARN("[route_cfg_apply] 无效的静态路由操作: %d", (int)apply->op);
            apply->rc = -1;
            break;
    }
}

// ============================================================================
// batch 路由应用
// ============================================================================

void route_cfg_apply_batch(route_apply_cmd_t *apply)
{
    if (!apply)
    {
        return;
    }

    switch (apply->op)
    {
        case ROUTE_APPLY_BATCH_ADD:
        {
            const char *name = apply->u.batch_add.name;
            uint32_t vrf_id = apply->u.batch_add.vrf_id;
            uint16_t afi = apply->u.batch_add.afi;
            uint8_t prefix_len = apply->u.batch_add.prefix_len;
            const net_addr_t *start_addr = &apply->u.batch_add.start_addr;
            int64_t count = apply->u.batch_add.count;
            const net_addr_t *nexthop_addr = &apply->u.batch_add.nexthop_addr;
            int32_t metric = apply->u.batch_add.metric;
            int32_t preference = apply->u.batch_add.preference;
            const char *out_ifname = apply->u.batch_add.out_ifname;

            int conflict =
                batch_check_conflict(name, vrf_id, afi, start_addr, prefix_len, count, apply->u.batch_add.conflict_name,
                                     sizeof(apply->u.batch_add.conflict_name));
            if (conflict < 0)
            {
                apply->rc = -1;
                break;
            }
            if (conflict > 0)
            {
                apply->rc = -2;
                break;
            }

            /* 若同名 batch 已存在，先清除其旧路由 */
            GList *l = g_route_work_local->batch_entries;
            while (l)
            {
                route_batch_entry_t *be = (route_batch_entry_t *)l->data;
                GList *next = l->next;
                if (strcmp(be->name, name) == 0)
                {
                    route_static_del(be->vrf_id, be->afi, &be->prefix_addr, be->prefix_len, &be->nexthop_addr,
                                     be->out_ifname);
                    g_route_work_local->batch_entries = g_list_delete_link(g_route_work_local->batch_entries, l);
                    g_free(be);
                }
                l = next;
            }

            int added = batch_do_add(name, vrf_id, afi, start_addr, prefix_len, count, nexthop_addr, metric, preference,
                                     out_ifname);
            route_recompute_iter_paths();
            apply->rc = added;
            break;
        }

        case ROUTE_APPLY_BATCH_DEL:
        {
            const char *name = apply->u.batch_del.name;
            int total = 0;

            GList *l = g_route_work_local->batch_entries;
            while (l)
            {
                route_batch_entry_t *be = (route_batch_entry_t *)l->data;
                GList *next = l->next;
                if (strcmp(be->name, name) == 0)
                {
                    int ret = route_static_del(be->vrf_id, be->afi, &be->prefix_addr, be->prefix_len, &be->nexthop_addr,
                                               be->out_ifname);
                    if (ret > 0)
                    {
                        total++;
                    }
                    g_route_work_local->batch_entries = g_list_delete_link(g_route_work_local->batch_entries, l);
                    g_free(be);
                }
                l = next;
            }

            route_recompute_iter_paths();
            apply->rc = total;
            break;
        }

        default:
            LOG_WARN("[route_cfg_apply] 无效的 batch 操作: %d", (int)apply->op);
            apply->rc = -1;
            break;
    }
}
