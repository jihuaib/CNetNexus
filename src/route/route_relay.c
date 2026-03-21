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
    uint8_t _pad[7];
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
    if (!head || !head->path_hash)
    {
        return NULL;
    }

    route_path_t *best = NULL;
    GHashTableIter iter;
    gpointer key_ptr = NULL;
    gpointer val_ptr = NULL;
    g_hash_table_iter_init(&iter, head->path_hash);
    while (g_hash_table_iter_next(&iter, &key_ptr, &val_ptr))
    {
        (void)key_ptr;
        route_path_t *path = (route_path_t *)val_ptr;
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

static int route_nh_is_resolved(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop)
{
    if (!rib || !nexthop)
    {
        return 0;
    }

    sa_family_t expected_family = 0;
    if (afi == ROUTE_AFI_IPV4)
    {
        expected_family = AF_INET;
    }
    else if (afi == ROUTE_AFI_IPV6)
    {
        expected_family = AF_INET6;
    }
    else
    {
        return 0;
    }

    if (nexthop->family != expected_family)
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

        route_path_t *resolver = route_lookup_best_cover(rib, vrf_id, afi, &cursor);
        if (!resolver)
        {
            return 0;
        }

        if (resolver->key.protocol == ROUTE_PROTOCOL_CONNECTED)
        {
            return 1;
        }
        if (resolver->nexthop.family == 0)
        {
            return 1;
        }
        if (resolver->nexthop.family != cursor.family)
        {
            return 0;
        }

        cursor = resolver->nexthop;
    }

    return 0;
}

static void route_relay_notify_state(dev_ipc_context_t *ctx, const route_nh_watch_t *watch)
{
    if (!watch)
    {
        return;
    }

    dev_ipc_context_t *send_ctx = ctx ? ctx : (g_route_local ? g_route_local->dev_ipc_ctx : NULL);
    if (!send_ctx)
    {
        return;
    }

    route_nh_iter_notify_t *payload = (route_nh_iter_notify_t *)g_malloc0(sizeof(*payload));
    if (!payload)
    {
        return;
    }

    payload->vrf_id = watch->key.vrf_id;
    payload->afi = watch->key.afi;
    payload->safi = watch->key.safi;
    payload->resolved = watch->resolved ? 1u : 0u;
    payload->nexthop_addr = watch->key.nexthop_addr;

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
    if (req->afi == ROUTE_AFI_IPV4)
    {
        return req->nexthop_addr.family == AF_INET;
    }
    if (req->afi == ROUTE_AFI_IPV6)
    {
        return req->nexthop_addr.family == AF_INET6;
    }
    return 0;
}

void route_relay_handle_nh_register(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
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
    watch->resolved = route_nh_is_resolved(g_route_local ? g_route_local->rib : NULL, watch->key.vrf_id, watch->key.afi,
                                           &watch->key.nexthop_addr)
                          ? 1u
                          : 0u;
    watch->updated_at_usec = g_get_real_time();

    if (is_new || old_resolved != watch->resolved)
    {
        route_relay_notify_state(ctx, watch);
    }

    dev_ipc_message_free(msg);
}

void route_relay_handle_nh_unregister(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
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
    dev_ipc_context_t *ctx;
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
    watch->resolved = route_nh_is_resolved(g_route_local ? g_route_local->rib : NULL, watch->key.vrf_id, watch->key.afi,
                                           &watch->key.nexthop_addr)
                          ? 1u
                          : 0u;
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
        route_relay_notify_state(ctx->ctx, watch);
    }

    return;
}

void route_recompute_iter_paths(dev_ipc_context_t *ctx)
{
    if (!g_route_nh_watch_table || g_hash_table_size(g_route_nh_watch_table) == 0)
    {
        return;
    }

    route_iter_recompute_ctx_t rctx = {
        .ctx = ctx ? ctx : (g_route_local ? g_route_local->dev_ipc_ctx : NULL),
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

void route_relay_cleanup(void)
{
    if (!g_route_nh_watch_table)
    {
        return;
    }
    g_hash_table_destroy(g_route_nh_watch_table);
    g_route_nh_watch_table = NULL;
}
