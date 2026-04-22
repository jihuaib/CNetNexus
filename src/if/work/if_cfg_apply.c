/**
 * @file   if_cfg_apply.c
 * @brief  接口配置内存态应用实现（CLI / DB 恢复共用）
 * @author jhb
 * @date   2026/03/08
 */
#include "if_cfg_apply.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "errcode.h"
#include "if.h"
#include "if_event.h"
#include "if_main.h"
#include "if_pub.h"
#include "if_worker.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

#define IF_ROUTE_SYNC_TIMEOUT_MS 3000u
#define IF_LOOP_IFINDEX_RETRY_COUNT 20u
#define IF_LOOP_IFINDEX_RETRY_USEC 50000u

// ============================================================================
// 公共 API
// ============================================================================

static uint32_t if_cfg_type_to_mask(if_type_t type)
{
    switch (type)
    {
        case IF_TYPE_ETHERNET:
        case IF_TYPE_VETH:
            /* 现阶段统一对外暴露为 ETH 口 */
            return IF_INTF_TYPE_ETH;
        default:
            return 0;
    }
}

static uint32_t if_cfg_wait_ifindex(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return 0u;
    }

    for (uint32_t i = 0u; i < IF_LOOP_IFINDEX_RETRY_COUNT; ++i)
    {
        uint32_t ifindex = (uint32_t)if_nametoindex(ifname);
        if (ifindex != 0u)
        {
            return ifindex;
        }
        (void)usleep(IF_LOOP_IFINDEX_RETRY_USEC);
    }
    return 0u;
}

static uint32_t if_cfg_resolve_entry_ifindex(if_map_entry_t *entry)
{
    if (!entry || entry->physical_name[0] == '\0')
    {
        return 0u;
    }

    uint32_t ifindex = if_cfg_wait_ifindex(entry->physical_name);
    if (ifindex != 0u)
    {
        entry->ifindex = ifindex;
        return ifindex;
    }
    return entry->ifindex;
}

static gboolean if_prefix_equal(const net_prefix_t *a, const net_prefix_t *b)
{
    if (!a || !b)
    {
        return FALSE;
    }
    return (a->prefix_len == b->prefix_len && net_addr_equal(&a->addr, &b->addr)) ? TRUE : FALSE;
}

static net_prefix_t *if_entry_prefix_by_family(if_map_entry_t *entry, sa_family_t family)
{
    if (!entry)
    {
        return NULL;
    }
    if (family == AF_INET)
    {
        return &entry->prefix_v4;
    }
    if (family == AF_INET6)
    {
        return &entry->prefix_v6;
    }
    return NULL;
}

static gboolean if_prefix_len_valid(const net_prefix_t *prefix)
{
    if (!prefix)
    {
        return FALSE;
    }
    if (prefix->addr.family == AF_INET)
    {
        return (prefix->prefix_len <= 32) ? TRUE : FALSE;
    }
    if (prefix->addr.family == AF_INET6)
    {
        return (prefix->prefix_len <= 128) ? TRUE : FALSE;
    }
    return FALSE;
}

static int if_prefix_to_network(const net_prefix_t *prefix, net_addr_t *out)
{
    if (!prefix || !out || !net_prefix_is_set(prefix))
    {
        return -1;
    }

    if (prefix->addr.family == AF_INET)
    {
        if (prefix->prefix_len > 32)
        {
            return -1;
        }
        uint32_t ip = ntohl(prefix->addr.u.v4.s_addr);
        uint32_t mask = (prefix->prefix_len == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix->prefix_len));
        memset(out, 0, sizeof(*out));
        out->family = AF_INET;
        out->u.v4.s_addr = htonl(ip & mask);
        return 0;
    }

    if (prefix->addr.family == AF_INET6)
    {
        if (prefix->prefix_len > 128)
        {
            return -1;
        }
        memset(out, 0, sizeof(*out));
        out->family = AF_INET6;
        memcpy(out->u.v6.s6_addr, prefix->addr.u.v6.s6_addr, 16);

        uint8_t bits = prefix->prefix_len;
        for (int i = 0; i < 16; i++)
        {
            if (bits >= 8)
            {
                bits -= 8;
                continue;
            }
            if (bits == 0)
            {
                out->u.v6.s6_addr[i] = 0;
            }
            else
            {
                uint8_t mask = (uint8_t)(0xFFu << (8 - bits));
                out->u.v6.s6_addr[i] &= mask;
                bits = 0;
            }
        }
        return 0;
    }

    return -1;
}

static void if_make_zero_addr(sa_family_t family, net_addr_t *out)
{
    if (!out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->family = family;
}

static void if_fill_connected_route_entry(route_msg_entry_t *entry, uint16_t afi, uint8_t prefix_len,
                                          const net_addr_t *prefix_addr, const net_addr_t *source_addr,
                                          const net_addr_t *nexthop_addr, uint32_t out_ifindex)
{
    if (!entry || !prefix_addr || !source_addr || !nexthop_addr)
    {
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->vrf_id = ROUTE_VRF_DEFAULT;
    entry->afi = afi;
    entry->safi = ROUTE_SAFI_UNICAST;
    entry->prefix_len = prefix_len;
    entry->protocol = ROUTE_PROTOCOL_CONNECTED;
    entry->metric = 0;
    entry->preference = ROUTE_ADMIN_DIST_CONNECTED;
    entry->is_withdraw = 0;
    entry->flags = 0;
    entry->out_ifindex = out_ifindex;
    entry->iter_out_ifindex = out_ifindex;
    entry->prefix_addr = *prefix_addr;
    entry->source_addr = *source_addr;
    entry->nexthop_addr = *nexthop_addr;
    entry->iter_nexthop_addr = *nexthop_addr;
}

static int if_sync_connected_host_routes(const net_prefix_t *prefix, const char *physical_name, gboolean is_withdraw)
{
    if (!prefix || !net_prefix_is_set(prefix) || !physical_name)
    {
        return ERRCODE_FAIL;
    }

    uint16_t afi = 0;
    if (prefix->addr.family == AF_INET)
    {
        afi = ROUTE_AFI_IPV4;
    }
    else if (prefix->addr.family == AF_INET6)
    {
        afi = ROUTE_AFI_IPV6;
    }
    else
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = g_if_local ? if_local_ipc_ctx() : NULL;
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    /* 普通接口：注入/撤销直连路由 */
    net_addr_t network_addr;
    if (if_prefix_to_network(prefix, &network_addr) != 0)
    {
        return ERRCODE_FAIL;
    }

    net_addr_t zero_nh;
    if_make_zero_addr(prefix->addr.family, &zero_nh);

    /* 出接口索引必须有效，避免将 ifindex=0 下发到路由模块。 */
    uint32_t out_ifindex = if_cfg_wait_ifindex(physical_name);
    if (out_ifindex == 0u)
    {
        LOG_ERROR("IF: interface %s ifindex invalid(0), skip connected route sync", physical_name);
        return ERRCODE_FAIL;
    }
    uint8_t host_len = (prefix->addr.family == AF_INET) ? 32u : 128u;

    route_msg_entry_t network_entry;
    if_fill_connected_route_entry(&network_entry, afi, prefix->prefix_len, &network_addr, &prefix->addr, &zero_nh,
                                  out_ifindex);

    /* 主机前缀（/32 或 /128）下，network 与 host 重合，只下发一条。 */
    if (prefix->prefix_len == host_len)
    {
        if (is_withdraw)
        {
            return route_rpc_del_wait(ctx, &network_entry, IF_ROUTE_SYNC_TIMEOUT_MS);
        }
        return route_rpc_add_wait(ctx, &network_entry, IF_ROUTE_SYNC_TIMEOUT_MS);
    }

    /* 非主机前缀：显式下发 host + network，避免 ROUTE 侧隐式派生 /32(/128) 导致 RIB 与 OS 不一致。 */
    route_msg_entry_t host_entry = network_entry;
    host_entry.prefix_addr = prefix->addr;
    host_entry.prefix_len = host_len;

    if (is_withdraw)
    {
        /* 先撤网段路由，再撤 host 路由。 */
        if (route_rpc_del_wait(ctx, &network_entry, IF_ROUTE_SYNC_TIMEOUT_MS) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
        if (route_rpc_del_wait(ctx, &host_entry, IF_ROUTE_SYNC_TIMEOUT_MS) != ERRCODE_SUCCESS)
        {
            (void)route_rpc_add_wait(ctx, &network_entry, IF_ROUTE_SYNC_TIMEOUT_MS);
            return ERRCODE_FAIL;
        }
        return ERRCODE_SUCCESS;
    }

    /* 先下发 host 路由，再下发 network 路由。 */
    if (route_rpc_add_wait(ctx, &host_entry, IF_ROUTE_SYNC_TIMEOUT_MS) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (route_rpc_add_wait(ctx, &network_entry, IF_ROUTE_SYNC_TIMEOUT_MS) != ERRCODE_SUCCESS)
    {
        (void)route_rpc_del_wait(ctx, &host_entry, IF_ROUTE_SYNC_TIMEOUT_MS);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

static int if_sync_connected_prefix(const net_prefix_t *prefix, const char *physical_name, gboolean is_withdraw)
{
    if (if_sync_connected_host_routes(prefix, physical_name, is_withdraw) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    int ret = is_withdraw ? if_addr_del_prefix(physical_name, prefix) : if_addr_add_prefix(physical_name, prefix);
    if (ret != ERRCODE_SUCCESS)
    {
        /* 地址下发失败时回滚 route 内存态，避免 RIB 与 OS 失配。 */
        (void)if_sync_connected_host_routes(prefix, physical_name, is_withdraw ? FALSE : TRUE);
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

if_map_entry_t *if_cfg_find_entry(const char *logical_name)
{
    if (!logical_name || !g_if_work_local)
    {
        return NULL;
    }

    if_map_t *map = &g_if_work_local->interface_map;

    if (!map->all_entries)
    {
        return NULL;
    }

    return (if_map_entry_t *)g_tree_lookup(map->all_entries, logical_name);
}

int if_cfg_loop_ensure(uint32_t loop_id)
{
    char name[32];
    snprintf(name, sizeof(name), "loop%u", loop_id);

    if (!g_if_work_local || !g_if_work_local->interface_map.all_entries)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *exist = (if_map_entry_t *)g_tree_lookup(g_if_work_local->interface_map.all_entries, name);
    if (exist)
    {
        if (exist->ifindex == 0u)
        {
            uint32_t ifindex = if_cfg_wait_ifindex(exist->physical_name);
            if (ifindex != 0u)
            {
                exist->ifindex = ifindex;
            }
            else
            {
                LOG_ERROR("IF: loop 接口 %s 已存在但 ifindex 无效(0)", name);
                return ERRCODE_FAIL;
            }
        }
        return ERRCODE_SUCCESS;
    }

    /* 创建 OS dummy 接口（若不存在） */
    int created_dummy = 0;
    if (!if_exists(name))
    {
        if (if_create_dummy(name) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("IF: 创建 dummy 接口 %s 失败", name);
            return ERRCODE_FAIL;
        }
        created_dummy = 1;
        if_set_state(name, 1);
    }

    uint32_t ifindex = if_cfg_wait_ifindex(name);
    if (ifindex == 0u)
    {
        LOG_ERROR("IF: loop 接口 %s ifindex 获取失败(0)", name);
        if (created_dummy)
        {
            (void)if_delete_interface(name);
        }
        return ERRCODE_FAIL;
    }

    /* 创建内存条目并插入有序树（key/value 均由树析构时释放） */
    if_map_entry_t *entry = (if_map_entry_t *)g_malloc0(sizeof(if_map_entry_t));
    snprintf(entry->logical_name, sizeof(entry->logical_name), "loop%u", loop_id);
    snprintf(entry->physical_name, sizeof(entry->physical_name), "loop%u", loop_id);
    entry->ifindex = ifindex;
    entry->shutdown = 0;
    entry->link_up = 1;

    g_tree_insert(g_if_work_local->interface_map.all_entries, g_strdup(name), entry);
    LOG_INFO("IF: loop 接口 %s 已创建（内存条目 ifindex=%u）", name, ifindex);

    uint32_t if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
    if (if_type != 0u)
    {
        if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_LINK_UP, 1u, NULL, 0u);
    }
    return ERRCODE_SUCCESS;
}

int if_cfg_loop_create(uint32_t loop_id)
{
    char name[32];
    snprintf(name, sizeof(name), "loop%u", loop_id);

    /* 已存在则视为成功（幂等） */
    if (if_cfg_find_entry(name))
    {
        LOG_DEBUG("IF: loop 接口 %s 已存在", name);
        return ERRCODE_SUCCESS;
    }

    if (if_cfg_loop_ensure(loop_id) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    /* 写入 DB */
    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "name", name);
    db_record_set_text(rec, "ip_address", "");
    db_record_set_int(rec, "prefix_len", 0);
    db_record_set_text(rec, "ipv6_address", "");
    db_record_set_int(rec, "ipv6_prefix_len", 0);
    db_record_set_int(rec, "shutdown", 0);
    db_rpc_insert_record(if_local_ipc_ctx(), "if_interface", rec);
    db_record_free(rec);

    LOG_INFO("IF: loop 接口 %s 已创建（内存 + DB）", name);
    return ERRCODE_SUCCESS;
}

int if_cfg_loop_delete(uint32_t loop_id)
{
    char name[32];
    snprintf(name, sizeof(name), "loop%u", loop_id);

    if_map_entry_t *entry = if_cfg_find_entry(name);
    if (!entry)
    {
        LOG_WARN("IF: loop 接口 %s 不存在，忽略删除", name);
        return ERRCODE_FAIL;
    }

    /* 撤销 IPv4/IPv6（若已配置） */
    if (net_prefix_is_set(&entry->prefix_v4))
    {
        net_prefix_t pfx = entry->prefix_v4;
        if_cfg_apply_ip(TRUE, name, &pfx);
    }
    if (net_prefix_is_set(&entry->prefix_v6))
    {
        net_prefix_t pfx = entry->prefix_v6;
        if_cfg_apply_ip(TRUE, name, &pfx);
    }

    uint32_t if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
    if (if_type != 0u)
    {
        if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_LINK_DOWN, 0u, NULL, 0u);
    }

    /* 删除 OS 接口 */
    if_delete_interface(name);

    /* 从有序树中移除（会触发 g_free 释放 key 和 value） */
    g_tree_remove(g_if_work_local->interface_map.all_entries, name);

    /* 从 DB 中删除 */
    db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(name)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_rpc_delete(if_local_ipc_ctx(), "if_interface", &filter);
    db_value_free(&cond.value);

    LOG_INFO("IF: loop 接口 %s 已删除", name);
    return ERRCODE_SUCCESS;
}

int if_cfg_apply_ip(gboolean is_no, const char *logical_name, const net_prefix_t *prefix)
{
    if (!logical_name)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_ERROR("IF: Interface %s not found", logical_name);
        return ERRCODE_FAIL;
    }

    sa_family_t family = 0;
    if (prefix && net_prefix_is_set(prefix))
    {
        family = prefix->addr.family;
    }

    if (is_no)
    {
        uint32_t del_if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
        uint32_t del_ifindex = 0u;
        if (del_if_type != 0u)
        {
            del_ifindex = if_cfg_resolve_entry_ifindex(entry);
            if (del_ifindex == 0u && strcmp(entry->logical_name, "null0") != 0)
            {
                LOG_ERROR("IF: %s ifindex invalid(0), skip address delete event", logical_name);
                return ERRCODE_FAIL;
            }
        }

        if (family == 0)
        {
            if (net_prefix_is_set(&entry->prefix_v4))
            {
                net_prefix_t old_v4 = entry->prefix_v4;
                if (!entry->shutdown &&
                    if_sync_connected_prefix(&entry->prefix_v4, entry->physical_name, TRUE) != ERRCODE_SUCCESS)
                {
                    return ERRCODE_FAIL;
                }
                memset(&entry->prefix_v4, 0, sizeof(entry->prefix_v4));
                if (del_if_type != 0)
                {
                    if_pub_notify(g_if_work_local->subscribers, entry, del_if_type, IF_EVENT_PROTO_DOWN, 0, &old_v4,
                                  del_ifindex);
                }
            }
            if (net_prefix_is_set(&entry->prefix_v6))
            {
                net_prefix_t old_v6 = entry->prefix_v6;
                if (!entry->shutdown &&
                    if_sync_connected_prefix(&entry->prefix_v6, entry->physical_name, TRUE) != ERRCODE_SUCCESS)
                {
                    return ERRCODE_FAIL;
                }
                memset(&entry->prefix_v6, 0, sizeof(entry->prefix_v6));
                if (del_if_type != 0)
                {
                    if_pub_notify(g_if_work_local->subscribers, entry, del_if_type, IF_EVENT_PROTO_DOWN, 0, &old_v6,
                                  del_ifindex);
                }
            }
            LOG_INFO("IF: %s all IP addresses cleared", logical_name);
            return ERRCODE_SUCCESS;
        }

        net_prefix_t *dst = if_entry_prefix_by_family(entry, family);
        if (!dst)
        {
            LOG_ERROR("IF: invalid address family for %s", logical_name);
            return ERRCODE_FAIL;
        }
        if (net_prefix_is_set(dst))
        {
            net_prefix_t old_pfx = *dst;
            if (!entry->shutdown && if_sync_connected_prefix(dst, entry->physical_name, TRUE) != ERRCODE_SUCCESS)
            {
                return ERRCODE_FAIL;
            }
            memset(dst, 0, sizeof(*dst));
            if (del_if_type != 0)
            {
                if_pub_notify(g_if_work_local->subscribers, entry, del_if_type, IF_EVENT_PROTO_DOWN, 0, &old_pfx,
                              del_ifindex);
            }
        }

        LOG_INFO("IF: %s %s address cleared", logical_name, (family == AF_INET6) ? "IPv6" : "IPv4");
        return ERRCODE_SUCCESS;
    }

    if (!prefix || !net_prefix_is_set(prefix) || !if_prefix_len_valid(prefix))
    {
        LOG_ERROR("IF: invalid prefix for %s", logical_name);
        return ERRCODE_FAIL;
    }

    char ip_str[64];
    net_addr_to_str(&prefix->addr, ip_str, sizeof(ip_str));

    net_prefix_t *dst = if_entry_prefix_by_family(entry, prefix->addr.family);
    if (!dst)
    {
        LOG_ERROR("IF: invalid address family for %s", logical_name);
        return ERRCODE_FAIL;
    }

    net_prefix_t old_prefix = *dst;
    gboolean had_old = net_prefix_is_set(&old_prefix);

    if (had_old && !if_prefix_equal(&old_prefix, prefix))
    {
        if (!entry->shutdown && if_sync_connected_prefix(&old_prefix, entry->physical_name, TRUE) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }

    *dst = *prefix;
    if (!entry->shutdown)
    {
        if (if_sync_connected_prefix(dst, entry->physical_name, FALSE) != ERRCODE_SUCCESS)
        {
            if (had_old && !if_prefix_equal(&old_prefix, prefix))
            {
                (void)if_sync_connected_prefix(&old_prefix, entry->physical_name, FALSE);
                *dst = old_prefix;
            }
            else if (!had_old)
            {
                memset(dst, 0, sizeof(*dst));
            }
            return ERRCODE_FAIL;
        }
    }

    /* 发布地址变更事件 */
    uint32_t add_if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
    if (add_if_type != 0)
    {
        uint32_t add_ifindex = if_cfg_resolve_entry_ifindex(entry);
        if (add_ifindex == 0u && strcmp(entry->logical_name, "null0") != 0)
        {
            LOG_ERROR("IF: %s ifindex invalid(0), skip address add event", logical_name);
            return ERRCODE_FAIL;
        }
        if (had_old && !if_prefix_equal(&old_prefix, prefix))
        {
            if_pub_notify(g_if_work_local->subscribers, entry, add_if_type, IF_EVENT_PROTO_DOWN, 0, &old_prefix,
                          add_ifindex);
        }
        if (!entry->shutdown)
        {
            if_pub_notify(g_if_work_local->subscribers, entry, add_if_type, IF_EVENT_PROTO_UP, 0, prefix, add_ifindex);
        }
    }

    LOG_INFO("IF: %s %s=%s/%u configured", logical_name, (prefix->addr.family == AF_INET6) ? "IPv6" : "IPv4", ip_str,
             prefix->prefix_len);
    return ERRCODE_SUCCESS;
}

int if_cfg_apply_shutdown(gboolean is_no, const char *logical_name)
{
    if (!logical_name)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_ERROR("IF: Interface %s not found", logical_name);
        return ERRCODE_FAIL;
    }

    /* is_no=TRUE → no shutdown → up；is_no=FALSE → shutdown → down */
    int up = is_no ? 1 : 0;
    int old_shutdown = entry->shutdown;

    if (if_set_state(entry->physical_name, up) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: Failed to set interface %s state", entry->physical_name);
        return ERRCODE_FAIL;
    }

    entry->shutdown = up ? 0 : 1;

    if (old_shutdown != entry->shutdown)
    {
        gboolean synced_v4 = FALSE;
        gboolean synced_v6 = FALSE;

        if (net_prefix_is_set(&entry->prefix_v4))
        {
            if (if_sync_connected_prefix(&entry->prefix_v4, entry->physical_name, up ? FALSE : TRUE) != ERRCODE_SUCCESS)
            {
                goto sync_rollback;
            }
            synced_v4 = TRUE;
        }
        if (net_prefix_is_set(&entry->prefix_v6))
        {
            if (if_sync_connected_prefix(&entry->prefix_v6, entry->physical_name, up ? FALSE : TRUE) != ERRCODE_SUCCESS)
            {
                goto sync_rollback;
            }
            synced_v6 = TRUE;
        }

        uint32_t if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
        /* `shutdown` command only changes protocol state. Physical link state reflects OS actual interface state */
        if (if_type != 0)
        {
            if (!up)
            {
                if (synced_v4)
                {
                    if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_PROTO_DOWN, 0,
                                  &entry->prefix_v4, entry->ifindex);
                }
                if (synced_v6)
                {
                    if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_PROTO_DOWN, 0,
                                  &entry->prefix_v6, entry->ifindex);
                }
            }
            else
            {
                if (synced_v4)
                {
                    if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_PROTO_UP, 0, &entry->prefix_v4,
                                  entry->ifindex);
                }
                if (synced_v6)
                {
                    if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_PROTO_UP, 0, &entry->prefix_v6,
                                  entry->ifindex);
                }
            }
        }
        goto sync_done;

    sync_rollback:
        if (synced_v6)
        {
            (void)if_sync_connected_prefix(&entry->prefix_v6, entry->physical_name, up ? TRUE : FALSE);
        }
        if (synced_v4)
        {
            (void)if_sync_connected_prefix(&entry->prefix_v4, entry->physical_name, up ? TRUE : FALSE);
        }
        (void)if_set_state(entry->physical_name, old_shutdown ? 0 : 1);
        entry->shutdown = old_shutdown;
        return ERRCODE_FAIL;

    sync_done:;
    }

    LOG_INFO("IF: %s %s", logical_name, up ? "no shutdown" : "shutdown");
    return ERRCODE_SUCCESS;
}

/* ============================================================================
 * Link Monitor 恢复 / 清理（由 IF work 线程调用）
 * ============================================================================ */

/**
 * @brief 使用保存的 ifindex 构造路由条目并从 RIB 中撤销直连路由
 *
 * 当接口被销毁时，if_nametoindex() 已无法获取 ifindex，需要用保存的旧值。
 */
static int if_withdraw_connected_with_ifindex(const net_prefix_t *prefix, uint32_t saved_ifindex)
{
    if (!prefix || !net_prefix_is_set(prefix) || saved_ifindex == 0u)
    {
        return ERRCODE_FAIL;
    }

    uint16_t afi = 0;
    if (prefix->addr.family == AF_INET)
    {
        afi = ROUTE_AFI_IPV4;
    }
    else if (prefix->addr.family == AF_INET6)
    {
        afi = ROUTE_AFI_IPV6;
    }
    else
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = g_if_local ? if_local_ipc_ctx() : NULL;
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    net_addr_t network_addr;
    if (if_prefix_to_network(prefix, &network_addr) != 0)
    {
        return ERRCODE_FAIL;
    }

    net_addr_t zero_nh;
    if_make_zero_addr(prefix->addr.family, &zero_nh);

    uint8_t host_len = (prefix->addr.family == AF_INET) ? 32u : 128u;

    route_msg_entry_t network_entry;
    if_fill_connected_route_entry(&network_entry, afi, prefix->prefix_len, &network_addr, &prefix->addr, &zero_nh,
                                  saved_ifindex);

    if (prefix->prefix_len == host_len)
    {
        return route_rpc_del_wait(ctx, &network_entry, IF_ROUTE_SYNC_TIMEOUT_MS);
    }

    route_msg_entry_t host_entry = network_entry;
    host_entry.prefix_addr = prefix->addr;
    host_entry.prefix_len = host_len;

    (void)route_rpc_del_wait(ctx, &network_entry, IF_ROUTE_SYNC_TIMEOUT_MS);
    (void)route_rpc_del_wait(ctx, &host_entry, IF_ROUTE_SYNC_TIMEOUT_MS);
    return ERRCODE_SUCCESS;
}

int if_cfg_recover_link(const char *logical_name, uint32_t new_ifindex, const net_prefix_t *pfx_v4,
                        const net_prefix_t *pfx_v6)
{
    if (!logical_name)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_WARN("IF-RECOVER: entry not found for %s", logical_name);
        return ERRCODE_FAIL;
    }

    /* 确保使用最新 ifindex */
    entry->ifindex = new_ifindex;

    /* 根据配置状态将新接口 UP 或 DOWN */
    if (if_set_state(entry->physical_name, entry->shutdown ? 0 : 1) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF-RECOVER: failed to change state for %s(%s)", logical_name, entry->physical_name);
    }

    /* 重新下发 IPv4 地址和直连路由 */
    if (pfx_v4 && net_prefix_is_set(pfx_v4))
    {
        LOG_INFO("IF-RECOVER: re-applying IPv4 on %s", logical_name);
        if (if_sync_connected_prefix(pfx_v4, entry->physical_name, FALSE) != ERRCODE_SUCCESS)
        {
            LOG_WARN("IF-RECOVER: IPv4 sync failed for %s", logical_name);
        }
    }

    /* 重新下发 IPv6 地址和直连路由 */
    if (pfx_v6 && net_prefix_is_set(pfx_v6))
    {
        LOG_INFO("IF-RECOVER: re-applying IPv6 on %s", logical_name);
        if (if_sync_connected_prefix(pfx_v6, entry->physical_name, FALSE) != ERRCODE_SUCCESS)
        {
            LOG_WARN("IF-RECOVER: IPv6 sync failed for %s", logical_name);
        }
    }

    LOG_INFO("IF-RECOVER: link %s recovered, ifindex=%u", logical_name, new_ifindex);
    return ERRCODE_SUCCESS;
}

void if_cfg_handle_link_down(const char *logical_name, uint32_t old_ifindex, const net_prefix_t *pfx_v4,
                             const net_prefix_t *pfx_v6)
{
    if (!logical_name)
    {
        return;
    }

    LOG_INFO("IF-LINKDOWN: withdrawing connected routes for %s (old ifindex=%u)", logical_name, old_ifindex);

    /* 使用保存的旧 ifindex 撤销直连路由 */
    if (pfx_v4 && net_prefix_is_set(pfx_v4))
    {
        if (if_withdraw_connected_with_ifindex(pfx_v4, old_ifindex) != ERRCODE_SUCCESS)
        {
            LOG_WARN("IF-LINKDOWN: IPv4 route withdrawal failed for %s", logical_name);
        }
    }

    if (pfx_v6 && net_prefix_is_set(pfx_v6))
    {
        if (if_withdraw_connected_with_ifindex(pfx_v6, old_ifindex) != ERRCODE_SUCCESS)
        {
            LOG_WARN("IF-LINKDOWN: IPv6 route withdrawal failed for %s", logical_name);
        }
    }
}
