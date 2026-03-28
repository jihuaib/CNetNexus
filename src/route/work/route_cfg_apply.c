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

/**
 * @brief 生成 batch 路由并写入静态候选表和 batch_entries 列表
 *
 * @param name       batch 名称
 * @param afi        地址族
 * @param start_addr 起始地址字符串
 * @param prefix_len 前缀长度
 * @param count      路由数量
 * @param nexthop    下一跳地址字符串
 * @return 成功添加数量；-1 地址解析失败
 */
static int batch_do_add(const char *name, uint16_t afi, const char *start_addr, uint8_t prefix_len, int64_t count,
                        const char *nexthop)
{
    int added = 0;

    net_addr_t nexthop_addr;
    if (net_addr_from_str(nexthop, &nexthop_addr) != 0)
    {
        return -1;
    }

    if (afi == ROUTE_AFI_IPV4)
    {
        struct in_addr base;
        if (inet_pton(AF_INET, start_addr, &base) != 1)
        {
            return -1;
        }

        uint32_t step = (prefix_len < 32) ? (1u << (32 - prefix_len)) : 1u;
        uint32_t addr = ntohl(base.s_addr);

        for (int64_t i = 0; i < count; i++)
        {
            struct in_addr cur;
            cur.s_addr = htonl(addr);

            net_addr_t prefix_addr;
            prefix_addr.family = AF_INET;
            prefix_addr.u.v4 = cur;

            route_static_add(ROUTE_VRF_DEFAULT, ROUTE_AFI_IPV4, &prefix_addr, prefix_len, &nexthop_addr, 0,
                             ROUTE_ADMIN_DIST_STATIC);

            route_batch_entry_t *be = (route_batch_entry_t *)g_malloc0(sizeof(route_batch_entry_t));
            g_strlcpy(be->name, name, sizeof(be->name));
            be->vrf_id = ROUTE_VRF_DEFAULT;
            be->afi = ROUTE_AFI_IPV4;
            be->prefix_len = prefix_len;
            be->prefix_addr = prefix_addr;
            be->nexthop_addr = nexthop_addr;
            g_route_work_local->batch_entries = g_list_append(g_route_work_local->batch_entries, be);

            addr += step;
            added++;
        }
    }
    else if (afi == ROUTE_AFI_IPV6)
    {
        uint8_t addr16[16];
        if (inet_pton(AF_INET6, start_addr, addr16) != 1)
        {
            return -1;
        }

        for (int64_t i = 0; i < count; i++)
        {
            struct in6_addr cur;
            memcpy(cur.s6_addr, addr16, 16);

            net_addr_t prefix_addr;
            prefix_addr.family = AF_INET6;
            prefix_addr.u.v6 = cur;

            route_static_add(ROUTE_VRF_DEFAULT, ROUTE_AFI_IPV6, &prefix_addr, prefix_len, &nexthop_addr, 0,
                             ROUTE_ADMIN_DIST_STATIC);

            route_batch_entry_t *be = (route_batch_entry_t *)g_malloc0(sizeof(route_batch_entry_t));
            g_strlcpy(be->name, name, sizeof(be->name));
            be->vrf_id = ROUTE_VRF_DEFAULT;
            be->afi = ROUTE_AFI_IPV6;
            be->prefix_len = prefix_len;
            be->prefix_addr = prefix_addr;
            be->nexthop_addr = nexthop_addr;
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
            int ret =
                route_static_add(apply->u.static_add.vrf_id, apply->u.static_add.afi, &apply->u.static_add.prefix_addr,
                                 apply->u.static_add.prefix_len, &apply->u.static_add.nexthop_addr,
                                 apply->u.static_add.metric, apply->u.static_add.preference);
            route_recompute_iter_paths();
            apply->rc = (ret >= 0) ? 1 : -1;
            break;
        }

        case ROUTE_APPLY_STATIC_DEL:
        {
            int ret =
                route_static_del(apply->u.static_del.vrf_id, apply->u.static_del.afi, &apply->u.static_del.prefix_addr,
                                 apply->u.static_del.prefix_len, &apply->u.static_del.nexthop_addr);
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
            uint16_t afi = apply->u.batch_add.afi;
            uint8_t prefix_len = apply->u.batch_add.prefix_len;
            const char *start_addr = apply->u.batch_add.start_addr;
            int64_t count = apply->u.batch_add.count;
            const char *nexthop = apply->u.batch_add.nexthop;

            /* 若同名 batch 已存在，先清除其旧路由 */
            GList *l = g_route_work_local->batch_entries;
            while (l)
            {
                route_batch_entry_t *be = (route_batch_entry_t *)l->data;
                GList *next = l->next;
                if (strcmp(be->name, name) == 0)
                {
                    route_static_del(be->vrf_id, be->afi, &be->prefix_addr, be->prefix_len, &be->nexthop_addr);
                    g_route_work_local->batch_entries = g_list_delete_link(g_route_work_local->batch_entries, l);
                    g_free(be);
                }
                l = next;
            }

            int added = batch_do_add(name, afi, start_addr, prefix_len, count, nexthop);
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
                    int ret =
                        route_static_del(be->vrf_id, be->afi, &be->prefix_addr, be->prefix_len, &be->nexthop_addr);
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
