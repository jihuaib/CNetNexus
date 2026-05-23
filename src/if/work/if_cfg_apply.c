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

#include "errcode.h"
#include "if.h"
#include "if_db.h"
#include "if_main.h"
#include "if_map.h"
#include "if_netlink.h"
#include "if_pub.h"
#include "if_worker.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "vrf.h"

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

static gboolean if_cfg_route_ready(void)
{
    return (g_if_work_local && g_if_work_local->route_ready) ? TRUE : FALSE;
}

static void if_prefix_log_str(const net_prefix_t *prefix, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0)
    {
        return;
    }
    buf[0] = '\0';
    if (!prefix || !net_prefix_is_set(prefix))
    {
        g_strlcpy(buf, "<unset>", buf_len);
        return;
    }
    net_prefix_to_str(prefix, buf, buf_len);
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

static gboolean if_physical_is_loopback(const char *physical_name)
{
    if (!physical_name)
    {
        return FALSE;
    }
    /* loop 前缀且后随数字（loop1/loop123 等）；与 if_bdr.c 中现有判定保持一致 */
    return (strncmp(physical_name, "loop", 4) == 0 && physical_name[4] >= '0' && physical_name[4] <= '9') ? TRUE
                                                                                                          : FALSE;
}

static int if_resolve_vrf_id(const char *vrf_name, uint32_t *vrf_id)
{
    if (!vrf_id)
    {
        return ERRCODE_FAIL;
    }

    *vrf_id = ROUTE_VRF_DEFAULT;
    if (!vrf_name || vrf_name[0] == '\0' || strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        return ERRCODE_SUCCESS;
    }

    const vrf_api_cache_entry_t *vrf = vrf_api_cache_lookup_by_name(vrf_name);
    if (!vrf)
    {
        LOG_ERROR("IF: VRF %s not found in cache, skip connected route sync", vrf_name);
        return ERRCODE_FAIL;
    }

    *vrf_id = vrf->vrf_id;
    return ERRCODE_SUCCESS;
}

static void if_fill_connected_route_entry(route_msg_entry_t *entry, uint16_t afi, uint8_t prefix_len,
                                          const net_addr_t *prefix_addr, const net_addr_t *source_addr,
                                          const net_addr_t *nexthop_addr, uint32_t out_ifindex,
                                          const char *physical_name, uint32_t vrf_id)
{
    if (!entry || !prefix_addr || !source_addr || !nexthop_addr)
    {
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->vrf_id = vrf_id;
    entry->afi = afi;
    entry->safi = ROUTE_SAFI_UNICAST;
    entry->prefix_len = prefix_len;
    entry->protocol = ROUTE_PROTOCOL_CONNECTED;
    entry->metric = 0;
    entry->preference = ROUTE_ADMIN_DIST_CONNECTED;
    entry->is_withdraw = 0;
    /* ETH/VETH 接口的直连路由仅用于本地转发可达性，不参与对外通告；
     * loopback 接口承载本地业务地址，需允许 BGP 等协议正常引入并发布。 */
    entry->flags = if_physical_is_loopback(physical_name) ? 0u : ROUTE_ENTRY_FLAG_NO_ADV;
    entry->out_ifindex = out_ifindex;
    entry->iter_out_ifindex = out_ifindex;
    entry->prefix_addr = *prefix_addr;
    entry->source_addr = *source_addr;
    entry->nexthop_addr = *nexthop_addr;
    entry->iter_nexthop_addr = *nexthop_addr;
}

static int if_sync_connected_host_routes(const if_map_entry_t *if_entry, const net_prefix_t *prefix,
                                         gboolean is_withdraw)
{
    if (!if_entry || !prefix || !net_prefix_is_set(prefix) || if_entry->physical_name[0] == '\0')
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

    uint32_t vrf_id = ROUTE_VRF_DEFAULT;
    if (if_resolve_vrf_id(if_entry->vrf_name, &vrf_id) != ERRCODE_SUCCESS)
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
    uint32_t out_ifindex = if_cfg_wait_ifindex(if_entry->physical_name);
    if (out_ifindex == 0u)
    {
        LOG_ERROR("IF: interface %s ifindex invalid(0), skip connected route sync", if_entry->physical_name);
        return ERRCODE_FAIL;
    }
    uint8_t host_len = (prefix->addr.family == AF_INET) ? 32u : 128u;

    route_msg_entry_t network_entry;
    if_fill_connected_route_entry(&network_entry, afi, prefix->prefix_len, &network_addr, &prefix->addr, &zero_nh,
                                  out_ifindex, if_entry->physical_name, vrf_id);

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

static int if_sync_connected_prefix(const if_map_entry_t *if_entry, const net_prefix_t *prefix, gboolean is_withdraw)
{
    if (!if_entry || !prefix || !net_prefix_is_set(prefix))
    {
        return ERRCODE_FAIL;
    }

    if (is_withdraw)
    {
        int os_rc = if_addr_del_prefix(if_entry->physical_name, prefix);
        if (!if_cfg_route_ready())
        {
            char pfx[80];
            if_prefix_log_str(prefix, pfx, sizeof(pfx));
            LOG_WARN("IF: ROUTE not ready, skipped connected route withdraw for %s on %s", pfx, if_entry->logical_name);
            return os_rc;
        }
        if (if_sync_connected_host_routes(if_entry, prefix, TRUE) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
        return os_rc;
    }

    if (!if_cfg_route_ready())
    {
        char pfx[80];
        if_prefix_log_str(prefix, pfx, sizeof(pfx));
        LOG_WARN("IF: ROUTE not ready, deferred OS address and connected route apply for %s on %s", pfx,
                 if_entry->logical_name);
        return ERRCODE_DEP_MISSING;
    }

    if (if_sync_connected_host_routes(if_entry, prefix, FALSE) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    int ret = if_addr_add_prefix(if_entry->physical_name, prefix);
    if (ret != ERRCODE_SUCCESS)
    {
        /* 地址下发失败时回滚 route 内存态，避免 RIB 与 OS 失配。 */
        (void)if_sync_connected_host_routes(if_entry, prefix, TRUE);
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

typedef struct if_replay_connected_ctx
{
    int replayed;
} if_replay_connected_ctx_t;

static gboolean if_replay_connected_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    if_map_entry_t *entry = (if_map_entry_t *)value;
    if_replay_connected_ctx_t *ctx = (if_replay_connected_ctx_t *)user_data;
    if (!entry || !ctx || entry->shutdown)
    {
        return FALSE;
    }

    uint32_t if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
    uint32_t ifindex = if_cfg_resolve_entry_ifindex(entry);

    if (net_prefix_is_set(&entry->prefix_v4) &&
        if_sync_connected_prefix(entry, &entry->prefix_v4, FALSE) == ERRCODE_SUCCESS)
    {
        ctx->replayed++;
        if (if_type != 0u && ifindex != 0u)
        {
            if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_PROTO_UP, 0, &entry->prefix_v4,
                          ifindex);
        }
    }
    if (net_prefix_is_set(&entry->prefix_v6) &&
        if_sync_connected_prefix(entry, &entry->prefix_v6, FALSE) == ERRCODE_SUCCESS)
    {
        ctx->replayed++;
        if (if_type != 0u && ifindex != 0u)
        {
            if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_PROTO_UP, 0, &entry->prefix_v6,
                          ifindex);
        }
    }
    return FALSE;
}

int if_cfg_replay_connected_routes(void)
{
    if (!g_if_work_local || !g_if_work_local->interface_map.all_entries)
    {
        return -1;
    }

    if_replay_connected_ctx_t ctx = {
        .replayed = 0,
    };
    g_tree_foreach(g_if_work_local->interface_map.all_entries, if_replay_connected_cb, &ctx);
    LOG_INFO("IF: replayed %d connected route prefix(es) to ROUTE", ctx.replayed);
    return ctx.replayed;
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

    /* DB 写入由 CLI 线程负责（if_cli.c handle_if_loop_entry / if_db_restore），worker 只动内存+OS */
    LOG_INFO("IF: loop 接口 %s 已创建（内存 + OS）", name);
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

    /* DB 删除由 CLI 线程负责（if_cli.c handle_if_loop_entry），worker 只动内存+OS */
    LOG_INFO("IF: loop 接口 %s 已删除（内存 + OS）", name);
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
                if (!entry->shutdown && if_sync_connected_prefix(entry, &entry->prefix_v4, TRUE) != ERRCODE_SUCCESS)
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
                if (!entry->shutdown && if_sync_connected_prefix(entry, &entry->prefix_v6, TRUE) != ERRCODE_SUCCESS)
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
            if (!entry->shutdown && if_sync_connected_prefix(entry, dst, TRUE) != ERRCODE_SUCCESS)
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
        if (!entry->shutdown && if_sync_connected_prefix(entry, &old_prefix, TRUE) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }

    *dst = *prefix;
    gboolean sync_applied = entry->shutdown ? FALSE : TRUE;
    if (!entry->shutdown)
    {
        int sync_rc = if_sync_connected_prefix(entry, dst, FALSE);
        if (sync_rc == ERRCODE_SUCCESS)
        {
            sync_applied = TRUE;
        }
        else if (sync_rc == ERRCODE_DEP_MISSING)
        {
            sync_applied = FALSE;
        }
        else
        {
            if (had_old && !if_prefix_equal(&old_prefix, prefix))
            {
                (void)if_sync_connected_prefix(entry, &old_prefix, FALSE);
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
        if (!entry->shutdown && sync_applied)
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

    /* 虚拟接口（如 null0）无对应 OS netdev，跳过 ioctl，仅维护内存态 */
    if (!if_map_is_virtual_entry(entry->logical_name))
    {
        if (if_set_state(entry->physical_name, up) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("IF: Failed to set interface %s state", entry->physical_name);
            return ERRCODE_FAIL;
        }
    }

    entry->shutdown = up ? 0 : 1;

    if (old_shutdown != entry->shutdown)
    {
        gboolean synced_v4 = FALSE;
        gboolean synced_v6 = FALSE;

        if (net_prefix_is_set(&entry->prefix_v4))
        {
            int sync_rc = if_sync_connected_prefix(entry, &entry->prefix_v4, up ? FALSE : TRUE);
            if (sync_rc == ERRCODE_SUCCESS)
            {
                synced_v4 = TRUE;
            }
            else if (sync_rc != ERRCODE_DEP_MISSING)
            {
                goto sync_rollback;
            }
        }
        if (net_prefix_is_set(&entry->prefix_v6))
        {
            int sync_rc = if_sync_connected_prefix(entry, &entry->prefix_v6, up ? FALSE : TRUE);
            if (sync_rc == ERRCODE_SUCCESS)
            {
                synced_v6 = TRUE;
            }
            else if (sync_rc != ERRCODE_DEP_MISSING)
            {
                goto sync_rollback;
            }
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
            (void)if_sync_connected_prefix(entry, &entry->prefix_v6, up ? TRUE : FALSE);
        }
        if (synced_v4)
        {
            (void)if_sync_connected_prefix(entry, &entry->prefix_v4, up ? TRUE : FALSE);
        }
        (void)if_set_state(entry->physical_name, old_shutdown ? 0 : 1);
        entry->shutdown = old_shutdown;
        return ERRCODE_FAIL;

    sync_done:;
    }

    LOG_INFO("IF: %s %s", logical_name, up ? "no shutdown" : "shutdown");
    return ERRCODE_SUCCESS;
}

int if_cfg_apply_vrf_binding(const char *logical_name, const char *vrf_name)
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

    if (strcmp(entry->logical_name, "null0") == 0)
    {
        LOG_ERROR("IF: null0 does not support VRF binding");
        return ERRCODE_FAIL;
    }

    const char *target_vrf = vrf_name ? vrf_name : "";
    if (strcmp(entry->vrf_name, target_vrf) == 0)
    {
        return ERRCODE_SUCCESS;
    }

    uint32_t master_ifindex = 0u;
    if (target_vrf[0] != '\0')
    {
        master_ifindex = (uint32_t)if_nametoindex(target_vrf);
        if (master_ifindex == 0u)
        {
            LOG_WARN("IF: VRF device %s not yet present for %s, caller may retry", target_vrf, logical_name);
            return ERRCODE_DEP_MISSING;
        }
    }

    /* VRF 切换语义：先清空地址和旧 connected route，再移动 OS master。 */
    if (net_prefix_is_set(&entry->prefix_v4) || net_prefix_is_set(&entry->prefix_v6))
    {
        if (if_cfg_apply_ip(TRUE, logical_name, NULL) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }

    if (!if_map_is_virtual_entry(entry->logical_name) &&
        if_set_master(entry->physical_name, master_ifindex) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    g_strlcpy(entry->vrf_name, target_vrf, sizeof(entry->vrf_name));

    uint32_t if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
    if (if_type != 0u)
    {
        if_info_t info;
        uint8_t link_up = 0u;
        if (if_get_info(entry->physical_name, &info) == ERRCODE_SUCCESS)
        {
            link_up = (info.state == IF_STATE_UP) ? 1u : 0u;
        }
        else if (entry->link_up > 0)
        {
            link_up = 1u;
        }
        if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_VRF_CHANGE, link_up, NULL, entry->ifindex);
    }

    LOG_INFO("IF: %s vrf forwarding %s", logical_name, target_vrf[0] ? target_vrf : "public");
    return ERRCODE_SUCCESS;
}

/* ============================================================================
 * VRF 删除级联解绑
 * ============================================================================ */

typedef struct
{
    const char *vrf_name;
    GPtrArray *names; /**< 命中的 logical_name g_strdup 列表 */
} if_vrf_del_collect_ctx_t;

static gboolean if_vrf_del_collect_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    if_map_entry_t *e = (if_map_entry_t *)value;
    if_vrf_del_collect_ctx_t *ctx = (if_vrf_del_collect_ctx_t *)user_data;
    if (!e || !ctx || !ctx->vrf_name)
    {
        return FALSE;
    }
    if (e->vrf_name[0] != '\0' && strcmp(e->vrf_name, ctx->vrf_name) == 0)
    {
        g_ptr_array_add(ctx->names, g_strdup(e->logical_name));
    }
    return FALSE;
}

int if_cfg_apply_vrf_deleted(const char *vrf_name)
{
    if (!vrf_name || vrf_name[0] == '\0')
    {
        return 0;
    }
    if (!g_if_work_local || !g_if_work_local->interface_map.all_entries)
    {
        return 0;
    }

    if_vrf_del_collect_ctx_t cctx = {.vrf_name = vrf_name, .names = g_ptr_array_new_with_free_func(g_free)};
    g_tree_foreach(g_if_work_local->interface_map.all_entries, if_vrf_del_collect_cb, &cctx);

    int unbound = 0;
    for (guint i = 0; i < cctx.names->len; i++)
    {
        const char *logical_name = (const char *)cctx.names->pdata[i];
        if_map_entry_t *entry = if_cfg_find_entry(logical_name);
        if (!entry)
        {
            continue;
        }

        /* 清掉 IPv4/IPv6 地址（内核 master 已消失，连同 IP 一并失效） */
        if (net_prefix_is_set(&entry->prefix_v4) || net_prefix_is_set(&entry->prefix_v6))
        {
            (void)if_cfg_apply_ip(TRUE, logical_name, NULL);
        }

        /* 内核 master 设备已随 VRF 销毁消失，无需再调用 if_set_master(0) */
        entry->vrf_name[0] = '\0';

        /* DB 同步 */
        if_db_update_vrf(logical_name, "");

        /* 通知订阅者：VRF_CHANGE 让 route 等模块刷新候选静态路由迭代关系 */
        uint32_t if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
        if (if_type != 0u)
        {
            if_info_t info;
            uint8_t link_up = 0u;
            if (if_get_info(entry->physical_name, &info) == ERRCODE_SUCCESS)
            {
                link_up = (info.state == IF_STATE_UP) ? 1u : 0u;
            }
            else if (entry->link_up > 0)
            {
                link_up = 1u;
            }
            if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_VRF_CHANGE, link_up, NULL,
                          entry->ifindex);
        }

        LOG_INFO("IF: %s unbound from deleted VRF %s -> public", logical_name, vrf_name);
        unbound++;
    }

    g_ptr_array_free(cctx.names, TRUE);
    return unbound;
}

/* ============================================================================
 * Link Monitor 恢复 / 清理（由 IF work 线程调用）
 * ============================================================================ */

/**
 * @brief 使用保存的 ifindex 构造路由条目并从 RIB 中撤销直连路由
 *
 * 当接口被销毁时，if_nametoindex() 已无法获取 ifindex，需要用保存的旧值。
 */
static int if_withdraw_connected_with_ifindex(const if_map_entry_t *if_entry, const net_prefix_t *prefix,
                                              uint32_t saved_ifindex)
{
    if (!if_entry || !prefix || !net_prefix_is_set(prefix) || saved_ifindex == 0u)
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

    uint32_t vrf_id = ROUTE_VRF_DEFAULT;
    if (if_resolve_vrf_id(if_entry->vrf_name, &vrf_id) != ERRCODE_SUCCESS)
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
                                  saved_ifindex, if_entry->physical_name, vrf_id);

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
        if (if_sync_connected_prefix(entry, pfx_v4, FALSE) != ERRCODE_SUCCESS)
        {
            LOG_WARN("IF-RECOVER: IPv4 sync failed for %s", logical_name);
        }
    }

    /* 重新下发 IPv6 地址和直连路由 */
    if (pfx_v6 && net_prefix_is_set(pfx_v6))
    {
        LOG_INFO("IF-RECOVER: re-applying IPv6 on %s", logical_name);
        if (if_sync_connected_prefix(entry, pfx_v6, FALSE) != ERRCODE_SUCCESS)
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

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_WARN("IF-LINKDOWN: entry not found for %s", logical_name);
        return;
    }

    LOG_INFO("IF-LINKDOWN: withdrawing connected routes for %s (old ifindex=%u)", logical_name, old_ifindex);

    /* logical_name 与 physical_name 在 loop/GE 直连场景下一致，足以判定是否为 loop。 */
    /* 使用保存的旧 ifindex 撤销直连路由 */
    if (pfx_v4 && net_prefix_is_set(pfx_v4))
    {
        if (if_withdraw_connected_with_ifindex(entry, pfx_v4, old_ifindex) != ERRCODE_SUCCESS)
        {
            LOG_WARN("IF-LINKDOWN: IPv4 route withdrawal failed for %s", logical_name);
        }
    }

    if (pfx_v6 && net_prefix_is_set(pfx_v6))
    {
        if (if_withdraw_connected_with_ifindex(entry, pfx_v6, old_ifindex) != ERRCODE_SUCCESS)
        {
            LOG_WARN("IF-LINKDOWN: IPv6 route withdrawal failed for %s", logical_name);
        }
    }
}
