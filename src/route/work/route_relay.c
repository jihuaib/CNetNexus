/**
 * @file   route_relay.c
 * @brief  Route nexthop 迭代 relay（仅注册 nexthop，不注册前缀路由）
 */
#include "route_relay.h"

#include <string.h>
#include <sys/socket.h>

#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_main.h"
#include "route_nhobj.h"
#include "route_static.h"
#include "route_worker.h"

#define ROUTE_ITER_NH_MAX_DEPTH 8u

typedef struct route_nh_watch_key
{
    uint32_t owner_module_id;
    uint32_t nexthop_id;
    uint8_t safi;
    uint8_t _pad0[3];
} route_nh_watch_key_t;

typedef struct route_nh_watch
{
    route_nh_watch_key_t key;
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t resolved;
    uint8_t _pad0;
    uint32_t out_ifindex; /**< 解析出的出接口索引 */
    net_addr_t nexthop_addr;
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
    h = h * 33u + (guint)k->nexthop_id;
    h = h * 33u + (guint)k->safi;
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

    return ka->owner_module_id == kb->owner_module_id && ka->nexthop_id == kb->nexthop_id && ka->safi == kb->safi;
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

static int route_path_is_self_recursive(const route_path_t *path, const net_addr_t *addr)
{
    if (!path || !addr || path->key.protocol == ROUTE_PROTOCOL_CONNECTED)
    {
        return 0;
    }
    route_nhobj_info_t info;
    if (route_nhobj_lookup(path->nexthop_id, &info) != 0)
    {
        return 0;
    }
    return net_addr_equal(&info.key.nexthop, addr);
}

static route_path_t *route_head_best_resolver_path(route_head_t *head, const net_addr_t *addr)
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
        if (route_path_is_self_recursive(path, addr))
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

    route_path_t *best = route_head_best_resolver_path(head, ctx->addr);
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
        /* 非直连解析路径：取其 nexthop（来自 nexthop 对象）继续递归 */
        route_nhobj_info_t rinfo;
        net_addr_t resolver_nh;
        memset(&resolver_nh, 0, sizeof(resolver_nh));
        if (route_nhobj_lookup(resolver->nexthop_id, &rinfo) == 0)
        {
            resolver_nh = rinfo.key.nexthop;
        }
        if (resolver_nh.family == 0)
        {
            return 1;
        }
        if (resolver_nh.family != AF_INET && resolver_nh.family != AF_INET6)
        {
            return 0;
        }

        cursor = resolver_nh;
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
        route_static_on_nh_change(watch->key.nexthop_id, watch->resolved, &watch->relay_addr, watch->out_ifindex);
        return;
    }

    dev_ipc_context_t *send_ctx = route_local_ipc_ctx();

    route_nh_iter_notify_t *payload = (route_nh_iter_notify_t *)g_malloc0(sizeof(*payload));
    if (!payload)
    {
        return;
    }

    payload->nexthop_id = watch->key.nexthop_id;
    payload->vrf_id = watch->vrf_id;
    payload->afi = watch->afi;
    payload->safi = watch->key.safi;
    payload->resolved = watch->resolved ? 1u : 0u;
    payload->out_ifindex = watch->out_ifindex;
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
        LOG_WARN("ROUTE nh-notify send failed: dst=0x%08X nhid=%u vrf=%u afi=%u", watch->key.owner_module_id,
                 watch->key.nexthop_id, watch->vrf_id, watch->afi);
    }
    dev_ipc_message_free(msg);
}

static int route_relay_validate_req(const route_nh_iter_req_t *req, route_nhobj_info_t *info_out)
{
    if (!req)
    {
        return 0;
    }
    if (req->safi != 0 && req->safi != ROUTE_SAFI_UNICAST)
    {
        return 0;
    }
    if (req->nexthop_id == 0u || !info_out || route_nhobj_lookup(req->nexthop_id, info_out) != ERRCODE_SUCCESS)
    {
        return 0;
    }
    if (info_out->key.afi != ROUTE_AFI_IPV4 && info_out->key.afi != ROUTE_AFI_IPV6)
    {
        return 0;
    }
    if (info_out->key.nh_type != ROUTE_NH_TYPE_IP)
    {
        return 0;
    }
    return info_out->key.nexthop.family == AF_INET || info_out->key.nexthop.family == AF_INET6;
}

static int route_relay_validate_unregister_req(const route_nh_iter_req_t *req)
{
    if (!req || req->nexthop_id == 0u)
    {
        return 0;
    }
    return (req->safi == 0 || req->safi == ROUTE_SAFI_UNICAST) ? 1 : 0;
}

static gboolean route_relay_refresh_watch(route_nh_watch_t *watch)
{
    if (!watch)
    {
        return FALSE;
    }

    uint32_t old_vrf_id = watch->vrf_id;
    uint16_t old_afi = watch->afi;
    uint8_t old_resolved = watch->resolved;
    uint32_t old_oif = watch->out_ifindex;
    net_addr_t old_nh = watch->nexthop_addr;
    net_addr_t old_relay = watch->relay_addr;

    route_nhobj_info_t info;
    memset(&info, 0, sizeof(info));
    if (route_nhobj_lookup(watch->key.nexthop_id, &info) != ERRCODE_SUCCESS || info.key.nexthop.family == 0)
    {
        watch->resolved = 0u;
        watch->out_ifindex = 0u;
        memset(&watch->relay_addr, 0, sizeof(watch->relay_addr));
        watch->updated_at_usec = g_get_real_time();
        return old_resolved != watch->resolved || old_oif != watch->out_ifindex ||
               !net_addr_equal(&old_relay, &watch->relay_addr);
    }

    watch->vrf_id = info.key.vrf_id;
    watch->afi = info.key.afi;
    watch->nexthop_addr = info.key.nexthop;

    net_addr_t gw;
    uint32_t oif = 0;
    memset(&gw, 0, sizeof(gw));
    int res = route_nh_resolve(g_route_work_local ? g_route_work_local->rib : NULL, watch->vrf_id, watch->afi,
                               &watch->nexthop_addr, &gw, &oif);
    watch->resolved = res ? 1u : 0u;
    watch->relay_addr = gw;
    watch->out_ifindex = oif;
    watch->updated_at_usec = g_get_real_time();

    if (watch->key.owner_module_id != DEV_MODULE_ID_ROUTE)
    {
        route_nhobj_set_relay(watch->key.nexthop_id, watch->resolved ? &watch->relay_addr : NULL,
                              watch->resolved ? watch->out_ifindex : 0u);
    }

    return old_vrf_id != watch->vrf_id || old_afi != watch->afi || old_resolved != watch->resolved ||
           old_oif != watch->out_ifindex || !net_addr_equal(&old_nh, &watch->nexthop_addr) ||
           !net_addr_equal(&old_relay, &watch->relay_addr);
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
    route_nhobj_info_t info;
    memset(&info, 0, sizeof(info));
    if (!route_relay_validate_req(req, &info))
    {
        LOG_WARN("ROUTE_NH_REGISTER invalid payload: src=0x%08X nhid=%u", msg->src_module_id, req->nexthop_id);
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
    key.nexthop_id = req->nexthop_id;
    key.safi = (req->safi == 0) ? ROUTE_SAFI_UNICAST : req->safi;

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
        watch->vrf_id = info.key.vrf_id;
        watch->afi = info.key.afi;
        watch->nexthop_addr = info.key.nexthop;
        g_hash_table_insert(g_route_nh_watch_table, &watch->key, watch);
        is_new = TRUE;
    }

    gboolean changed = route_relay_refresh_watch(watch);
    if (is_new || changed)
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
    if (!route_relay_validate_unregister_req(req))
    {
        dev_ipc_message_free(msg);
        return;
    }

    if (g_route_nh_watch_table)
    {
        route_nh_watch_key_t key;
        memset(&key, 0, sizeof(key));
        key.owner_module_id = msg->src_module_id;
        key.nexthop_id = req->nexthop_id;
        key.safi = (req->safi == 0) ? ROUTE_SAFI_UNICAST : req->safi;
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

    gboolean changed = route_relay_refresh_watch(watch);

    ctx->total++;
    if (watch->resolved)
    {
        ctx->resolved++;
    }
    if (changed)
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

void route_relay_publish_unreachable_for_shutdown(void)
{
    if (!g_route_nh_watch_table)
    {
        return;
    }
    uint32_t notified = 0;
    GHashTableIter it;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&it, g_route_nh_watch_table);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        (void)key;
        route_nh_watch_t *w = (route_nh_watch_t *)val;
        if (!w || w->key.owner_module_id == DEV_MODULE_ID_ROUTE)
        {
            /* 自模块注册的 nh(供静态路由迭代用)走 static_on_nh_change 内部回调,
             * 退出阶段不需要 IPC 通知。 */
            continue;
        }
        /* 临时把 watch 标为不可达,通过现有 notify_state 把 NH_NOTIFY(resolved=0) 发出去。
         * route_nh_watch_table 紧随其后会被 route_relay_cleanup 整张销毁,
         * 这里直接改字段不会影响后续逻辑。 */
        w->resolved = 0u;
        memset(&w->relay_addr, 0, sizeof(w->relay_addr));
        w->out_ifindex = 0u;
        w->updated_at_usec = g_get_real_time();
        route_relay_notify_state(w);
        notified++;
    }
    if (notified > 0)
    {
        LOG_INFO("[route_relay] shutdown: notified %u external nh-watcher(s) as unreachable", notified);
    }
}

int route_relay_register_direct(uint32_t nexthop_id, uint32_t owner_module_id, net_addr_t *gateway_out,
                                uint32_t *ifindex_out)
{
    route_nhobj_info_t info;
    memset(&info, 0, sizeof(info));
    if (nexthop_id == 0u || route_nhobj_lookup(nexthop_id, &info) != ERRCODE_SUCCESS || info.key.nexthop.family == 0)
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
    key.nexthop_id = nexthop_id;
    key.safi = ROUTE_SAFI_UNICAST;

    route_nh_watch_t *watch = (route_nh_watch_t *)g_hash_table_lookup(g_route_nh_watch_table, &key);
    if (!watch)
    {
        watch = (route_nh_watch_t *)g_malloc0(sizeof(*watch));
        if (!watch)
        {
            return 0;
        }
        watch->key = key;
        watch->vrf_id = info.key.vrf_id;
        watch->afi = info.key.afi;
        watch->nexthop_addr = info.key.nexthop;
        g_hash_table_insert(g_route_nh_watch_table, &watch->key, watch);
    }

    (void)route_relay_refresh_watch(watch);

    if (gateway_out)
    {
        *gateway_out = watch->relay_addr;
    }
    if (ifindex_out)
    {
        *ifindex_out = watch->out_ifindex;
    }

    return (int)watch->resolved;
}

void route_relay_unregister_direct(uint32_t nexthop_id, uint32_t owner_module_id)
{
    if (!g_route_nh_watch_table || nexthop_id == 0u)
    {
        return;
    }

    route_nh_watch_key_t key;
    memset(&key, 0, sizeof(key));
    key.owner_module_id = owner_module_id;
    key.nexthop_id = nexthop_id;
    key.safi = ROUTE_SAFI_UNICAST;

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
    uint32_t vrf_filter;
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
    if (ctx->has_afi_filter && watch->afi != ctx->afi_filter)
    {
        return;
    }
    if (watch->vrf_id != ctx->vrf_filter)
    {
        return;
    }

    char nh_str[64];
    net_addr_to_str(&watch->nexthop_addr, nh_str, sizeof(nh_str));

    const char *afi_str = (watch->afi == ROUTE_AFI_IPV4) ? "ipv4" : (watch->afi == ROUTE_AFI_IPV6) ? "ipv6" : "?";
    const char *safi_str = (watch->key.safi == ROUTE_SAFI_UNICAST) ? "unicast" : "?";
    const char *resolved_str = watch->resolved ? "yes" : "no";

    g_string_append_printf(ctx->buf, "0x%08X  %-10u  %4u  %-4s  %-7s  %-20s  %s\r\n", watch->key.owner_module_id,
                           watch->key.nexthop_id, watch->vrf_id, afi_str, safi_str, nh_str, resolved_str);
    ctx->count++;
}

void route_relay_show(GString *buf, uint32_t module_filter, int has_filter, uint16_t afi_filter, int has_afi_filter,
                      uint32_t vrf_filter, const char *vrf_name)
{
    if (!buf)
    {
        return;
    }

    g_string_append_printf(buf, "\r\nRoute Relay (VRF: %s)\r\n", vrf_name ? vrf_name : "public");
    g_string_append_printf(buf,
                           "\r\n%-10s  %-10s  %4s  %-4s  %-7s  %-20s  %s\r\n"
                           "----------  ----------  ----  ----  -------  --------------------  --------\r\n",
                           "Module", "NexthopID", "VRF", "AFI", "SAFI", "Nexthop", "Resolved");

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
        .vrf_filter = vrf_filter,
        .count = 0,
    };

    g_hash_table_foreach(g_route_nh_watch_table, relay_show_cb, &ctx);

    if (ctx.count == 0)
    {
        g_string_append(buf, "  (no entries)\r\n");
    }

    g_string_append_printf(buf, "\r\nTotal %u entry\r\n", ctx.count);
}
