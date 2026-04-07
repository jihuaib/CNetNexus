/**
 * @file   route_relay.c
 * @brief  Route nexthop 迭代 relay（仅注册 nexthop，不注册前缀路由）
 */
#include "route_relay.h"

#include <string.h>
#include <sys/socket.h>

#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_main.h"
#include "route_static.h"
#include "route_worker.h"

#define ROUTE_ITER_NH_MAX_DEPTH 8u

typedef struct route_nh_watch_key
{
    uint32_t owner_module_id;
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t safi;
    uint8_t _pad0;
    net_addr_t nexthop_addr;
} route_nh_watch_key_t;

typedef struct route_nh_watch
{
    route_nh_watch_key_t key;
    uint8_t resolved;
    uint8_t _pad[3];
    uint32_t out_ifindex;  /**< 解析出的出接口索引 */
    net_addr_t relay_addr; /**< 解析出的 relay 地址 */
    gint64 updated_at_usec;
} route_nh_watch_t;

static GHashTable *g_route_nh_watch_table = NULL;

static guint route_nh_watch_key_hash(gconstpointer p)
{
    const route_nh_watch_key_t *k = (const route_nh_watch_key_t *)p;
    if (!k)
    {
        return 0;
    }

    guint h = (guint)k->owner_module_id;
    h = h * 33u + (guint)k->vrf_id;
    h = h * 33u + (guint)k->afi;
    h = h * 33u + (guint)k->safi;
    h ^= net_addr_hash(&k->nexthop_addr);
    return h;
}

static gboolean route_nh_watch_key_equal(gconstpointer a, gconstpointer b)
{
    const route_nh_watch_key_t *ka = (const route_nh_watch_key_t *)a;
    const route_nh_watch_key_t *kb = (const route_nh_watch_key_t *)b;
    if (!ka || !kb)
    {
        return FALSE;
    }

    return ka->owner_module_id == kb->owner_module_id && ka->vrf_id == kb->vrf_id && ka->afi == kb->afi &&
           ka->safi == kb->safi && net_addr_equal(&ka->nexthop_addr, &kb->nexthop_addr);
}

static void route_nh_watch_table_ensure(void)
{
    if (!g_route_nh_watch_table)
    {
        g_route_nh_watch_table = g_hash_table_new_full(route_nh_watch_key_hash, route_nh_watch_key_equal, NULL, g_free);
    }
}

static int route_prefix_contains_addr(const route_head_t *head, const net_addr_t *addr)
{
    if (!head || !addr || head->key.addr.family != addr->family)
    {
        return 0;
    }

    const uint8_t *pfx = NULL;
    const uint8_t *ip = NULL;
    uint8_t max_len = 0;

    if (addr->family == AF_INET)
    {
        pfx = (const uint8_t *)&head->key.addr.u.v4;
        ip = (const uint8_t *)&addr->u.v4;
        max_len = 32u;
    }
    else if (addr->family == AF_INET6)
    {
        pfx = (const uint8_t *)&head->key.addr.u.v6;
        ip = (const uint8_t *)&addr->u.v6;
        max_len = 128u;
    }
    else
    {
        return 0;
    }

    if (head->key.prefix_len > max_len)
    {
        return 0;
    }

    uint8_t full_bytes = (uint8_t)(head->key.prefix_len / 8u);
    uint8_t rem_bits = (uint8_t)(head->key.prefix_len % 8u);

    if (full_bytes > 0 && memcmp(pfx, ip, full_bytes) != 0)
    {
        return 0;
    }
    if (rem_bits > 0)
    {
        uint8_t mask = (uint8_t)(0xFFu << (8u - rem_bits));
        if ((pfx[full_bytes] & mask) != (ip[full_bytes] & mask))
        {
            return 0;
        }
    }

    return 1;
}

/* 返回值：<0 表示 a 优于 b；>0 表示 b 优于 a；0 表示同优 */
static int route_path_rank_cmp(const route_path_t *a, const route_path_t *b)
{
    if (!a && !b)
    {
        return 0;
    }
    if (!a)
    {
        return 1;
    }
    if (!b)
    {
        return -1;
    }

    if (a->preference != b->preference)
    {
        return (a->preference < b->preference) ? -1 : 1;
    }
    if (a->metric != b->metric)
    {
        return (a->metric < b->metric) ? -1 : 1;
    }
    if (a->updated_at_usec != b->updated_at_usec)
    {
        return (a->updated_at_usec > b->updated_at_usec) ? -1 : 1;
    }
    if (a->key.protocol != b->key.protocol)
    {
        return (a->key.protocol < b->key.protocol) ? -1 : 1;
    }
    return net_addr_cmp(&a->key.source, &b->key.source);
}

static route_path_t *route_head_best_resolver_path(route_head_t *head)
{
    if (!head || !head->path_list)
    {
        return NULL;
    }

    route_path_t *best = NULL;
    for (GList *l = head->path_list; l; l = l->next)
    {
        route_path_t *path = (route_path_t *)l->data;
        if (!path)
        {
            continue;
        }
        if (!best || route_path_rank_cmp(path, best) < 0)
        {
            best = path;
        }
    }

    return best;
}

typedef struct route_cover_lookup_ctx
{
    uint32_t vrf_id;
    uint16_t afi;
    const net_addr_t *addr;
    route_head_t *best_head;
    route_path_t *best_path;
} route_cover_lookup_ctx_t;

static gboolean route_cover_lookup_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    route_head_t *head = (route_head_t *)value;
    route_cover_lookup_ctx_t *ctx = (route_cover_lookup_ctx_t *)user_data;
    if (!head || !ctx || !ctx->addr)
    {
        return FALSE;
    }

    if (head->key.vrf_id != ctx->vrf_id || head->key.afi != ctx->afi)
    {
        return FALSE;
    }
    if (!route_prefix_contains_addr(head, ctx->addr))
    {
        return FALSE;
    }

    route_path_t *best = route_head_best_resolver_path(head);
    if (!best)
    {
        return FALSE;
    }

    if (!ctx->best_head)
    {
        ctx->best_head = head;
        ctx->best_path = best;
        return FALSE;
    }

    if (head->key.prefix_len > ctx->best_head->key.prefix_len)
    {
        ctx->best_head = head;
        ctx->best_path = best;
        return FALSE;
    }

    if (head->key.prefix_len == ctx->best_head->key.prefix_len && route_path_rank_cmp(best, ctx->best_path) < 0)
    {
        ctx->best_head = head;
        ctx->best_path = best;
    }

    return FALSE;
}

static route_path_t *route_lookup_best_cover(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *addr)
{
    if (!rib || !rib->head_tree || !addr)
    {
        return NULL;
    }

    route_cover_lookup_ctx_t ctx = {
        .vrf_id = vrf_id,
        .afi = afi,
        .addr = addr,
        .best_head = NULL,
        .best_path = NULL,
    };

    g_tree_foreach(rib->head_tree, route_cover_lookup_cb, &ctx);
    return ctx.best_path;
}

/**
 * @brief 一次迭代完成 nexthop 可达性判断 + 网关/出接口解析
 *
 * @param rib          RIB
 * @param vrf_id       VRF ID
 * @param afi          地址族
 * @param nexthop      下一跳地址
 * @param gateway_out  解析出的直连网关（可为 NULL 表示不需要）
 * @param ifindex_out  解析出的出接口索引（可为 NULL）
 * @return 1=可达，0=不可达
 */
static int route_nh_resolve(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop,
                            net_addr_t *gateway_out, uint32_t *ifindex_out)
{
    if (!rib || !nexthop)
    {
        return 0;
    }

    if (afi != ROUTE_AFI_IPV4 && afi != ROUTE_AFI_IPV6)
    {
        return 0;
    }

    /* nexthop 迭代按 nexthop 自身地址族查找覆盖路由：
     * 允许跨族场景（如 IPv4 前缀使用 IPv6 nexthop）。 */
    uint16_t resolve_afi = 0;
    if (nexthop->family == AF_INET)
    {
        resolve_afi = ROUTE_AFI_IPV4;
    }
    else if (nexthop->family == AF_INET6)
    {
        resolve_afi = ROUTE_AFI_IPV6;
    }
    else
    {
        return 0;
    }

    net_addr_t cursor = *nexthop;
    net_addr_t visited[ROUTE_ITER_NH_MAX_DEPTH];
    uint32_t visited_count = 0;

    for (uint32_t depth = 0; depth < ROUTE_ITER_NH_MAX_DEPTH; ++depth)
    {
        for (uint32_t i = 0; i < visited_count; ++i)
        {
            if (net_addr_equal(&visited[i], &cursor))
            {
                return 0;
            }
        }
        visited[visited_count++] = cursor;

        route_path_t *resolver = route_lookup_best_cover(rib, vrf_id, resolve_afi, &cursor);
        if (!resolver)
        {
            return 0;
        }

        if (resolver->key.protocol == ROUTE_PROTOCOL_CONNECTED)
        {
            /*
             * 仅当已解析出有效出接口时才认为可达。
             * 否则属于接口路由尚未就绪（例如 IF 侧稍后才补齐 ifindex）的中间态，
             * 不能放行静态/迭代路由进入 RIB。
             */
            if (resolver->out_ifindex == 0)
            {
                return 0;
            }
            /*
             * RIB 中存在 connected 路径但尚未成功下发 OS 时，仍视为不可达。
             * 避免静态/迭代路由抢在依赖直连路由前下发，触发 ENETUNREACH。
             */
            if ((resolver->flags & ROUTE_PATH_FLAG_OS_INSTALLED) == 0)
            {
                return 0;
            }
            if (gateway_out)
            {
                /*
                 * 直连解析的网关地址 = 当前迭代光标（即被解析的 nexthop 地址）。
                 * 不能使用 resolver->key.source，那是直连路由本端地址（如 10.12.0.1），
                 * 而非对端可达网关（如 10.12.0.2）。
                 */
                *gateway_out = cursor;
            }
            if (ifindex_out)
            {
                *ifindex_out = resolver->out_ifindex;
            }
            return 1;
        }
        if (resolver->nexthop.family == 0)
        {
            return 1;
        }
        if (resolver->nexthop.family != AF_INET && resolver->nexthop.family != AF_INET6)
        {
            return 0;
        }

        cursor = resolver->nexthop;
        resolve_afi = (cursor.family == AF_INET) ? ROUTE_AFI_IPV4 : ROUTE_AFI_IPV6;
    }

    return 0;
}

static void route_relay_notify_state(const route_nh_watch_t *watch)
{
    if (!watch)
    {
        return;
    }

    /* 自模块注册的 nexthop（静态路由）：走统一回调流程，不发 IPC */
    if (watch->key.owner_module_id == DEV_MODULE_ID_ROUTE)
    {
        route_static_on_nh_change(watch->key.vrf_id, watch->key.afi, &watch->key.nexthop_addr, watch->resolved,
                                  &watch->relay_addr, watch->out_ifindex);
        return;
    }

    dev_ipc_context_t *send_ctx = route_local_ipc_ctx();

    route_nh_iter_notify_t *payload = (route_nh_iter_notify_t *)g_malloc0(sizeof(*payload));
    if (!payload)
    {
        return;
    }

    payload->vrf_id = watch->key.vrf_id;
    payload->afi = watch->key.afi;
    payload->safi = watch->key.safi;
    payload->resolved = watch->resolved ? 1u : 0u;
    payload->out_ifindex = watch->out_ifindex;
    payload->nexthop_addr = watch->key.nexthop_addr;
    payload->relay_addr = watch->relay_addr;

    dev_ipc_message_t *msg =
        dev_ipc_message_create(ROUTE_MSG_TYPE_NH_NOTIFY, DEV_MODULE_ID_ROUTE, watch->key.owner_module_id, 0, payload,
                               sizeof(route_nh_iter_notify_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return;
    }

    if (dev_ipc_send(send_ctx, watch->key.owner_module_id, msg) != 0)
    {
        LOG_WARN("ROUTE nh-notify send failed: dst=0x%08X vrf=%u afi=%u", watch->key.owner_module_id, watch->key.vrf_id,
                 watch->key.afi);
    }
    dev_ipc_message_free(msg);
}

static int route_relay_validate_req(const route_nh_iter_req_t *req)
{
    if (!req)
    {
        return 0;
    }
    if (req->safi != 0 && req->safi != ROUTE_SAFI_UNICAST)
    {
        return 0;
    }
    if (req->afi != ROUTE_AFI_IPV4 && req->afi != ROUTE_AFI_IPV6)
    {
        return 0;
    }
    /* 允许 v4/v6 任意 nexthop 家族，具体可达性由 route_nh_resolve 判定。 */
    return req->nexthop_addr.family == AF_INET || req->nexthop_addr.family == AF_INET6;
}

void route_relay_handle_nh_register(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(route_nh_iter_req_t))
    {
        LOG_WARN("ROUTE_NH_REGISTER payload too short: %u", msg ? msg->payload_len : 0u);
        if (msg)
        {
            dev_ipc_message_free(msg);
        }
        return;
    }

    const route_nh_iter_req_t *req = (const route_nh_iter_req_t *)msg->payload;
    if (!route_relay_validate_req(req))
    {
        LOG_WARN("ROUTE_NH_REGISTER invalid payload: src=0x%08X vrf=%u afi=%u", msg->src_module_id, req->vrf_id,
                 req->afi);
        dev_ipc_message_free(msg);
        return;
    }

    route_nh_watch_table_ensure();
    if (!g_route_nh_watch_table)
    {
        dev_ipc_message_free(msg);
        return;
    }

    route_nh_watch_key_t key;
    memset(&key, 0, sizeof(key));
    key.owner_module_id = msg->src_module_id;
    key.vrf_id = req->vrf_id;
    key.afi = req->afi;
    key.safi = (req->safi == 0) ? ROUTE_SAFI_UNICAST : req->safi;
    key.nexthop_addr = req->nexthop_addr;

    route_nh_watch_t *watch = (route_nh_watch_t *)g_hash_table_lookup(g_route_nh_watch_table, &key);
    gboolean is_new = FALSE;
    if (!watch)
    {
        watch = (route_nh_watch_t *)g_malloc0(sizeof(*watch));
        if (!watch)
        {
            dev_ipc_message_free(msg);
            return;
        }
        watch->key = key;
        g_hash_table_insert(g_route_nh_watch_table, &watch->key, watch);
        is_new = TRUE;
    }

    uint8_t old_resolved = watch->resolved;
    net_addr_t gw;
    uint32_t oif = 0;
    memset(&gw, 0, sizeof(gw));
    int res = route_nh_resolve(g_route_work_local ? g_route_work_local->rib : NULL, watch->key.vrf_id, watch->key.afi,
                               &watch->key.nexthop_addr, &gw, &oif);
    watch->resolved = res ? 1u : 0u;
    watch->relay_addr = gw;
    watch->out_ifindex = oif;
    watch->updated_at_usec = g_get_real_time();

    if (is_new || old_resolved != watch->resolved)
    {
        route_relay_notify_state(watch);
    }

    dev_ipc_message_free(msg);
}

void route_relay_handle_nh_unregister(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(route_nh_iter_req_t))
    {
        if (msg)
        {
            dev_ipc_message_free(msg);
        }
        return;
    }

    const route_nh_iter_req_t *req = (const route_nh_iter_req_t *)msg->payload;
    if (!route_relay_validate_req(req))
    {
        dev_ipc_message_free(msg);
        return;
    }

    if (g_route_nh_watch_table)
    {
        route_nh_watch_key_t key;
        memset(&key, 0, sizeof(key));
        key.owner_module_id = msg->src_module_id;
        key.vrf_id = req->vrf_id;
        key.afi = req->afi;
        key.safi = (req->safi == 0) ? ROUTE_SAFI_UNICAST : req->safi;
        key.nexthop_addr = req->nexthop_addr;
        g_hash_table_remove(g_route_nh_watch_table, &key);
    }

    dev_ipc_message_free(msg);
}

typedef struct route_iter_recompute_ctx
{
    uint32_t total;
    uint32_t resolved;
    uint32_t announced;
    uint32_t withdrawn;
} route_iter_recompute_ctx_t;

static void route_recompute_watch_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    route_nh_watch_t *watch = (route_nh_watch_t *)value;
    route_iter_recompute_ctx_t *ctx = (route_iter_recompute_ctx_t *)user_data;
    if (!watch || !ctx)
    {
        return;
    }

    uint8_t old_resolved = watch->resolved;
    net_addr_t gw;
    uint32_t oif = 0;
    memset(&gw, 0, sizeof(gw));
    int res = route_nh_resolve(g_route_work_local ? g_route_work_local->rib : NULL, watch->key.vrf_id, watch->key.afi,
                               &watch->key.nexthop_addr, &gw, &oif);
    watch->resolved = res ? 1u : 0u;
    watch->relay_addr = gw;
    watch->out_ifindex = oif;
    watch->updated_at_usec = g_get_real_time();

    ctx->total++;
    if (watch->resolved)
    {
        ctx->resolved++;
    }
    if (old_resolved != watch->resolved)
    {
        if (watch->resolved)
        {
            ctx->announced++;
        }
        else
        {
            ctx->withdrawn++;
        }
        route_relay_notify_state(watch);
    }
}

void route_recompute_iter_paths(void)
{
    if (g_route_nh_watch_table && g_hash_table_size(g_route_nh_watch_table) > 0)
    {
        route_iter_recompute_ctx_t rctx = {
            .total = 0u,
            .resolved = 0u,
            .announced = 0u,
            .withdrawn = 0u,
        };

        g_hash_table_foreach(g_route_nh_watch_table, route_recompute_watch_cb, &rctx);

        if (rctx.total > 0 || rctx.announced > 0 || rctx.withdrawn > 0)
        {
            LOG_DEBUG("Route nh-watch recompute: total=%u resolved=%u up=%u down=%u", rctx.total, rctx.resolved,
                      rctx.announced, rctx.withdrawn);
        }
    }

    /* 重检查 interface-only 静态路由（基于 connected 路由状态判断接口可达性） */
    route_static_on_if_change();
}

void route_relay_cleanup(void)
{
    if (!g_route_nh_watch_table)
    {
        return;
    }
    g_hash_table_destroy(g_route_nh_watch_table);
    g_route_nh_watch_table = NULL;
}

int route_relay_register_direct(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr, uint32_t owner_module_id,
                                net_addr_t *gateway_out, uint32_t *ifindex_out)
{
    if (!nexthop_addr)
    {
        return 0;
    }

    route_nh_watch_table_ensure();
    if (!g_route_nh_watch_table)
    {
        return 0;
    }

    route_nh_watch_key_t key;
    memset(&key, 0, sizeof(key));
    key.owner_module_id = owner_module_id;
    key.vrf_id = vrf_id;
    key.afi = afi;
    key.safi = ROUTE_SAFI_UNICAST;
    key.nexthop_addr = *nexthop_addr;

    route_nh_watch_t *watch = (route_nh_watch_t *)g_hash_table_lookup(g_route_nh_watch_table, &key);
    if (!watch)
    {
        watch = (route_nh_watch_t *)g_malloc0(sizeof(*watch));
        if (!watch)
        {
            return 0;
        }
        watch->key = key;
        g_hash_table_insert(g_route_nh_watch_table, &watch->key, watch);
    }

    net_addr_t gw;
    uint32_t oif = 0;
    memset(&gw, 0, sizeof(gw));
    int res =
        route_nh_resolve(g_route_work_local ? g_route_work_local->rib : NULL, vrf_id, afi, nexthop_addr, &gw, &oif);
    watch->resolved = res ? 1u : 0u;
    watch->relay_addr = gw;
    watch->out_ifindex = oif;
    watch->updated_at_usec = g_get_real_time();

    if (gateway_out)
    {
        *gateway_out = gw;
    }
    if (ifindex_out)
    {
        *ifindex_out = oif;
    }

    return (int)watch->resolved;
}

void route_relay_unregister_direct(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr,
                                   uint32_t owner_module_id)
{
    if (!g_route_nh_watch_table || !nexthop_addr)
    {
        return;
    }

    route_nh_watch_key_t key;
    memset(&key, 0, sizeof(key));
    key.owner_module_id = owner_module_id;
    key.vrf_id = vrf_id;
    key.afi = afi;
    key.safi = ROUTE_SAFI_UNICAST;
    key.nexthop_addr = *nexthop_addr;

    g_hash_table_remove(g_route_nh_watch_table, &key);
}

// ============================================================================
// show route relay
// ============================================================================

typedef struct
{
    GString *buf;
    uint32_t module_filter;
    int has_filter;
    uint16_t afi_filter;
    int has_afi_filter;
    uint32_t count;
} relay_show_ctx_t;

static void relay_show_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    route_nh_watch_t *watch = (route_nh_watch_t *)value;
    relay_show_ctx_t *ctx = (relay_show_ctx_t *)user_data;
    if (!watch || !ctx)
    {
        return;
    }

    /* 按模块过滤 */
    if (ctx->has_filter && watch->key.owner_module_id != ctx->module_filter)
    {
        return;
    }

    /* 按 AFI 过滤 */
    if (ctx->has_afi_filter && watch->key.afi != ctx->afi_filter)
    {
        return;
    }

    char nh_str[64];
    net_addr_to_str(&watch->key.nexthop_addr, nh_str, sizeof(nh_str));

    const char *afi_str = (watch->key.afi == ROUTE_AFI_IPV4)   ? "ipv4"
                          : (watch->key.afi == ROUTE_AFI_IPV6) ? "ipv6"
                                                               : "?";
    const char *safi_str = (watch->key.safi == ROUTE_SAFI_UNICAST) ? "unicast" : "?";
    const char *resolved_str = watch->resolved ? "yes" : "no";

    g_string_append_printf(ctx->buf, "0x%08X  %4u  %-4s  %-7s  %-20s  %s\r\n", watch->key.owner_module_id,
                           watch->key.vrf_id, afi_str, safi_str, nh_str, resolved_str);
    ctx->count++;
}

void route_relay_show(GString *buf, uint32_t module_filter, int has_filter, uint16_t afi_filter, int has_afi_filter)
{
    if (!buf)
    {
        return;
    }

    g_string_append_printf(buf,
                           "\r\n%-10s  %4s  %-4s  %-7s  %-20s  %s\r\n"
                           "----------  ----  ----  -------  --------------------  --------\r\n",
                           "Module", "VRF", "AFI", "SAFI", "Nexthop", "Resolved");

    if (!g_route_nh_watch_table || g_hash_table_size(g_route_nh_watch_table) == 0)
    {
        g_string_append(buf, "  (no entries)\r\n");
        g_string_append(buf, "\r\nTotal 0 entry\r\n");
        return;
    }

    relay_show_ctx_t ctx = {
        .buf = buf,
        .module_filter = module_filter,
        .has_filter = has_filter,
        .afi_filter = afi_filter,
        .has_afi_filter = has_afi_filter,
        .count = 0,
    };

    g_hash_table_foreach(g_route_nh_watch_table, relay_show_cb, &ctx);

    if (ctx.count == 0)
    {
        g_string_append(buf, "  (no entries)\r\n");
    }

    g_string_append_printf(buf, "\r\nTotal %u entry\r\n", ctx.count);
}
