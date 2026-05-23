/**
 * @file   route_calc.c
 * @brief  Route 优选模块实现：多协议路径管理距离优选，唯一下发 FIB 并通知订阅者
 * @author jhb
 * @date   2026/03/24
 */
#include "route_calc.h"

#include <glib.h>
#include <netinet/in.h>
#include <string.h>

#include "errcode.h"
#include "fib.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_main.h"
#include "route_pub.h"
#include "route_rib.h"
#include "route_worker.h"

#define ROUTE_CALC_OS_RETRY_BASE_MSEC 200u
#define ROUTE_CALC_OS_RETRY_MAX_MSEC 2000u

typedef struct calc_retry_state
{
    gint64 due_usec;
    uint32_t delay_msec;
    uint32_t attempts;
} calc_retry_state_t;

static GHashTable *g_calc_os_retry_table = NULL;

// ============================================================================
// 辅助：path_key 比较 / 当前已下发路径查找
// ============================================================================

static void fill_head_key(route_head_key_t *dst, const route_head_t *head)
{
    memset(dst, 0, sizeof(*dst));
    dst->vrf_id = head->key.vrf_id;
    dst->afi = head->key.afi;
    dst->prefix_len = head->key.prefix_len;
    dst->addr = head->key.addr;
}

static guint calc_retry_key_hash(gconstpointer key)
{
    const route_head_key_t *k = (const route_head_key_t *)key;
    guint h = 0u;
    h ^= k->vrf_id * 2654435761u;
    h ^= ((guint)k->afi << 16) ^ (guint)k->prefix_len;
    h ^= net_addr_hash(&k->addr) * 31u;
    return h;
}

static gboolean calc_retry_key_equal(gconstpointer a, gconstpointer b)
{
    const route_head_key_t *ka = (const route_head_key_t *)a;
    const route_head_key_t *kb = (const route_head_key_t *)b;
    return ka->vrf_id == kb->vrf_id && ka->afi == kb->afi && ka->prefix_len == kb->prefix_len &&
           net_addr_equal(&ka->addr, &kb->addr);
}

static void calc_retry_table_ensure(void)
{
    if (!g_calc_os_retry_table)
    {
        g_calc_os_retry_table =
            g_hash_table_new_full(calc_retry_key_hash, calc_retry_key_equal, g_free, (GDestroyNotify)g_free);
    }
}

static void clear_os_install_retry(const route_head_t *head)
{
    if (!head || !g_calc_os_retry_table)
    {
        return;
    }

    route_head_key_t key;
    fill_head_key(&key, head);
    g_hash_table_remove(g_calc_os_retry_table, &key);
}

static void schedule_os_install_retry(const route_head_t *head)
{
    if (!head)
    {
        return;
    }

    calc_retry_table_ensure();
    if (!g_calc_os_retry_table)
    {
        return;
    }

    route_head_key_t key;
    fill_head_key(&key, head);
    calc_retry_state_t *state = (calc_retry_state_t *)g_hash_table_lookup(g_calc_os_retry_table, &key);
    gint64 now = g_get_real_time();

    if (state)
    {
        uint32_t next = state->delay_msec ? state->delay_msec : ROUTE_CALC_OS_RETRY_BASE_MSEC;
        if (next < ROUTE_CALC_OS_RETRY_MAX_MSEC)
        {
            next <<= 1;
            if (next > ROUTE_CALC_OS_RETRY_MAX_MSEC)
            {
                next = ROUTE_CALC_OS_RETRY_MAX_MSEC;
            }
        }
        state->delay_msec = next;
        state->attempts++;
        state->due_usec = now + (gint64)next * 1000;
        return;
    }

    route_head_key_t *stored_key = (route_head_key_t *)g_malloc(sizeof(*stored_key));
    calc_retry_state_t *new_state = (calc_retry_state_t *)g_malloc0(sizeof(*new_state));
    if (!stored_key || !new_state)
    {
        g_free(stored_key);
        g_free(new_state);
        return;
    }

    *stored_key = key;
    new_state->delay_msec = ROUTE_CALC_OS_RETRY_BASE_MSEC;
    new_state->attempts = 1u;
    new_state->due_usec = now + (gint64)new_state->delay_msec * 1000;
    g_hash_table_insert(g_calc_os_retry_table, stored_key, new_state);
}

static int path_key_same(const route_path_key_t *a, const route_path_key_t *b)
{
    return a->protocol == b->protocol && net_addr_equal(&a->source, &b->source);
}

/* 查找前缀下当前标记为 OS-installed 的路径（理论上最多一条） */
static route_path_t *find_os_installed_path(route_head_t *head)
{
    if (!head)
    {
        return NULL;
    }

    for (GList *l = head->path_list; l; l = l->next)
    {
        route_path_t *path = (route_path_t *)l->data;
        if (path && (path->flags & ROUTE_PATH_FLAG_OS_INSTALLED))
        {
            return path;
        }
    }
    return NULL;
}

/* 同步前缀下路径的 OS-installed 标记：仅 installed_key 对应路径置位，其余清零 */
static void sync_os_installed_flag(route_head_t *head, const route_path_key_t *installed_key)
{
    if (!head)
    {
        return;
    }
    for (GList *l = head->path_list; l; l = l->next)
    {
        route_path_t *path = (route_path_t *)l->data;
        if (!path)
        {
            continue;
        }
        if (installed_key && path_key_same(&path->key, installed_key))
        {
            path->flags |= ROUTE_PATH_FLAG_OS_INSTALLED;
        }
        else
        {
            path->flags &= ~ROUTE_PATH_FLAG_OS_INSTALLED;
        }
    }
}

// ============================================================================
// 辅助：从 head + path 构建路由条目
//   - 协议上报（UPDATE/REPORT）使用原始 nexthop
//   - FIB 下发使用 relay 解析后的 nexthop
// ============================================================================

static void build_report_entry(route_msg_entry_t *e, const route_head_t *head, const route_path_t *path)
{
    memset(e, 0, sizeof(*e));
    e->vrf_id = head->key.vrf_id;
    e->afi = (uint16_t)head->key.afi;
    e->safi = (uint8_t)ROUTE_SAFI_UNICAST;
    e->prefix_len = head->key.prefix_len;
    e->prefix_addr = head->key.addr;
    e->protocol = path->key.protocol;
    e->metric = path->metric;
    e->preference = path->preference;
    e->out_ifindex = path->out_ifindex;
    e->iter_out_ifindex = path->iter_out_ifindex;
    e->nexthop_addr = path->nexthop;
    e->iter_nexthop_addr = path->relay_addr;
    e->source_addr = path->key.source;
    e->is_withdraw = 0;
    /* 透传 path->entry_flags（如 ROUTE_ENTRY_FLAG_NO_ADV），由上层模块根据语义决定是否对外发布 */
    e->flags = (uint8_t)(path->entry_flags & 0xFFu);
    e->nh_type = path->nh_type ? path->nh_type : ROUTE_NH_TYPE_IP;
    e->tunnel_id = (e->nh_type == ROUTE_NH_TYPE_TUNNEL) ? path->tunnel_id : 0u;
}

static void build_os_entry(route_msg_entry_t *e, const route_head_t *head, const route_path_t *path)
{
    build_report_entry(e, head, path);
    if (e->iter_nexthop_addr.family == 0)
    {
        e->iter_nexthop_addr = e->nexthop_addr;
    }
    if (e->iter_out_ifindex == 0)
    {
        e->iter_out_ifindex = e->out_ifindex;
    }
}

static void build_fib_entry(fib_route_entry_t *fib, const route_msg_entry_t *route)
{
    memset(fib, 0, sizeof(*fib));
    fib->vrf_id = route->vrf_id;
    fib->afi = route->afi;
    fib->safi = route->safi;
    fib->prefix_len = route->prefix_len;
    fib->protocol = route->protocol;
    fib->metric = route->metric;
    fib->preference = route->preference;
    fib->flags = (route->protocol == ROUTE_PROTOCOL_CONNECTED) ? FIB_ROUTE_FLAG_SKIP_OS : 0u;
    if (route->protocol == ROUTE_PROTOCOL_BLACKHOLE)
    {
        fib->nh_type = FIB_NH_TYPE_BLACKHOLE;
    }
    else if (route->nh_type == ROUTE_NH_TYPE_TUNNEL && route->tunnel_id != 0u)
    {
        fib->nh_type = FIB_NH_TYPE_TUNNEL;
        fib->tunnel_id = route->tunnel_id;
    }
    else
    {
        fib->nh_type = FIB_NH_TYPE_IP;
    }
    fib->out_ifindex = (route->iter_out_ifindex != 0) ? route->iter_out_ifindex : route->out_ifindex;
    fib->prefix_addr = route->prefix_addr;
    fib->nexthop_addr = (route->iter_nexthop_addr.family == AF_INET || route->iter_nexthop_addr.family == AF_INET6)
                            ? route->iter_nexthop_addr
                            : route->nexthop_addr;
}

static int route_calc_fib_upsert(const route_msg_entry_t *route)
{
    fib_route_entry_t fib;
    build_fib_entry(&fib, route);
    return fib_rpc_route_upsert(route_local_ipc_ctx(), &fib);
}

static int route_calc_fib_delete(const route_msg_entry_t *route)
{
    fib_route_entry_t fib;
    build_fib_entry(&fib, route);
    return fib_rpc_route_delete(route_local_ipc_ctx(), &fib);
}

// ============================================================================
// 辅助：在 path_list 中选最优路径
//   - preference 越小越优先（管理距离）
//   - preference 相等时 metric 越小越优先
//   - skip_key != NULL 时跳过该路径（用于路径删除前的重算场景）
// ============================================================================

static const route_path_t *select_best_path(const route_head_t *head, const route_path_key_t *skip_key)
{
    const route_path_t *best = NULL;
    if (!head)
    {
        return NULL;
    }

    for (GList *l = head->path_list; l; l = l->next)
    {
        const route_path_t *path = (const route_path_t *)l->data;
        if (!path)
        {
            continue;
        }

        /* 跳过待删路径 */
        if (skip_key && path_key_same(&path->key, skip_key))
        {
            continue;
        }

        if (!best || path->preference < best->preference ||
            (path->preference == best->preference && path->metric < best->metric))
        {
            best = path;
        }
    }
    return best;
}

// ============================================================================
// 内部：重算前缀最优路径，同步 FIB 并通知订阅者
//
// 通知语义：订阅者只接收"当前最优路径"的变更事件。
//   - 最优路径切换：先发旧路径的 withdraw，再发新路径的 add
//   - 属性更新（nexthop/metric/ifindex 变化）：发新路径的 add（upsert 语义）
//   - 路径消失且无替代：发旧路径的 withdraw
// ============================================================================

static void update_prefix(const route_head_t *head, const route_path_key_t *skip_key)
{
    if (!head)
    {
        return;
    }

    route_head_t *mut_head = (route_head_t *)head;
    GList *subscribers = g_route_work_local ? g_route_work_local->subscribers : NULL;

    const route_path_t *new_best = select_best_path(head, skip_key);
    route_path_t *cur_installed = find_os_installed_path(mut_head);

    /* 情形 1：前缀下已无可用路径 */
    if (!new_best)
    {
        clear_os_install_retry(mut_head);
        if (cur_installed)
        {
            route_msg_entry_t cur_entry;
            route_msg_entry_t cur_os_entry;
            char addr_str[64];

            build_report_entry(&cur_entry, head, cur_installed);
            build_os_entry(&cur_os_entry, head, cur_installed);
            net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
            LOG_INFO("[route_calc] %s/%u vrf=%u 无可用路径，撤销 FIB 及通知: proto=%u", addr_str,
                     (unsigned)head->key.prefix_len, head->key.vrf_id, cur_installed->key.protocol);

            if (route_calc_fib_delete(&cur_os_entry) != 0)
            {
                LOG_WARN("[route_calc] FIB 撤销失败，保留当前最优状态: %s/%u vrf=%u proto=%u", addr_str,
                         (unsigned)head->key.prefix_len, head->key.vrf_id, cur_installed->key.protocol);
                return;
            }
            if (subscribers)
            {
                cur_entry.is_withdraw = 1;
                route_pub_notify_entry(subscribers, &cur_entry);
            }
        }
        sync_os_installed_flag(mut_head, NULL);
        return;
    }

    route_msg_entry_t new_entry;
    route_msg_entry_t new_os_entry;
    build_report_entry(&new_entry, head, new_best);
    build_os_entry(&new_os_entry, head, new_best);

    /* 情形 2：当前已安装的就是最优路径（同协议同来源） */
    if (cur_installed && path_key_same(&cur_installed->key, &new_best->key))
    {
        char addr_str[64];
        net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
        LOG_DEBUG("[route_calc] 最优路径更新: %s/%u vrf=%u proto=%u", addr_str, (unsigned)head->key.prefix_len,
                  head->key.vrf_id, new_best->key.protocol);

        if (route_calc_fib_upsert(&new_os_entry) != 0)
        {
            LOG_WARN("[route_calc] FIB 更新失败，保持旧最优: %s/%u vrf=%u proto=%u", addr_str,
                     (unsigned)head->key.prefix_len, head->key.vrf_id, new_best->key.protocol);
            schedule_os_install_retry(mut_head);
            return;
        }
        if (subscribers)
        {
            route_pub_notify_entry(subscribers, &new_entry);
        }
        route_rib_promote_path_first(mut_head, (route_path_t *)new_best);
        sync_os_installed_flag(mut_head, &new_best->key);
        clear_os_install_retry(mut_head);
        return;
    }

    /* 情形 3：最优路径切换或首次安装 */
    if (cur_installed)
    {
        route_msg_entry_t cur_entry;
        char addr_str[64];
        build_report_entry(&cur_entry, head, cur_installed);

        net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
        LOG_INFO("[route_calc] 最优路径切换: %s/%u vrf=%u proto=%u(pref=%d) -> proto=%u(pref=%d)", addr_str,
                 (unsigned)head->key.prefix_len, head->key.vrf_id, cur_installed->key.protocol, cur_entry.preference,
                 new_best->key.protocol, new_best->preference);

        if (route_calc_fib_upsert(&new_os_entry) != 0)
        {
            LOG_WARN("[route_calc] FIB 安装失败，保持当前最优不变: %s/%u vrf=%u proto=%u", addr_str,
                     (unsigned)head->key.prefix_len, head->key.vrf_id, new_best->key.protocol);
            schedule_os_install_retry(mut_head);
            return;
        }
        /* 不再显式撤销旧最优：当前下发不携带 RTA_PRIORITY，REPLACE 语义由内核覆盖同前缀主项。 */

        if (subscribers)
        {
            cur_entry.is_withdraw = 1;
            route_pub_notify_entry(subscribers, &cur_entry);
            route_pub_notify_entry(subscribers, &new_entry);
        }
    }
    else
    {
        char addr_str[64];
        net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
        LOG_INFO("[route_calc] 安装最优路由: %s/%u vrf=%u proto=%u pref=%d metric=%d", addr_str,
                 (unsigned)head->key.prefix_len, head->key.vrf_id, new_best->key.protocol, new_best->preference,
                 new_best->metric);

        if (route_calc_fib_upsert(&new_os_entry) != 0)
        {
            LOG_WARN("[route_calc] FIB 安装失败，保持当前最优不变: %s/%u vrf=%u proto=%u", addr_str,
                     (unsigned)head->key.prefix_len, head->key.vrf_id, new_best->key.protocol);
            schedule_os_install_retry(mut_head);
            return;
        }

        if (subscribers)
        {
            route_pub_notify_entry(subscribers, &new_entry);
        }
    }

    route_rib_promote_path_first(mut_head, (route_path_t *)new_best);
    sync_os_installed_flag(mut_head, &new_best->key);
    clear_os_install_retry(mut_head);
}

// ============================================================================
// 生命周期
// ============================================================================

void route_calc_init(void)
{
    if (g_calc_os_retry_table)
    {
        g_hash_table_destroy(g_calc_os_retry_table);
        g_calc_os_retry_table = NULL;
    }
    LOG_INFO("[route_calc] 优选状态初始化完成（基于 route_path FIB 标记）");
}

typedef struct
{
    uint32_t total;
    uint32_t withdrawn;
    uint32_t failed;
} calc_cleanup_ctx_t;

static void cleanup_withdraw_fib_cb(const route_head_t *head, const route_path_t *path, void *userdata)
{
    calc_cleanup_ctx_t *ctx = (calc_cleanup_ctx_t *)userdata;
    if (!head || !path || !ctx)
    {
        return;
    }

    ctx->total++;

    route_msg_entry_t os_entry;
    char addr_str[64];
    build_os_entry(&os_entry, head, path);
    net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));

    if (route_calc_fib_delete(&os_entry) == 0)
    {
        ctx->withdrawn++;
    }
    else
    {
        ctx->failed++;
        LOG_WARN("[route_calc] 退出清理删除 FIB 路由失败: %s/%u vrf=%u proto=%u", addr_str,
                 (unsigned)head->key.prefix_len, head->key.vrf_id, path->key.protocol);
    }
}

void route_calc_cleanup(void)
{
    if (g_route_work_local && g_route_work_local->rib)
    {
        calc_cleanup_ctx_t ctx = {
            .total = 0u,
            .withdrawn = 0u,
            .failed = 0u,
        };

        route_rib_walk(g_route_work_local->rib, ROUTE_PROTOCOL_MAX, ROUTE_VRF_ALL, cleanup_withdraw_fib_cb, &ctx);

        if (ctx.total > 0 || ctx.failed > 0)
        {
            LOG_INFO("[route_calc] 退出清理完成：fib=%u deleted=%u failed=%u", ctx.total, ctx.withdrawn, ctx.failed);
        }
    }

    if (g_calc_os_retry_table)
    {
        g_hash_table_destroy(g_calc_os_retry_table);
        g_calc_os_retry_table = NULL;
    }

    LOG_INFO("[route_calc] 优选状态已清理");
}

// ============================================================================
// 公共接口：RIB 变更通知
// ============================================================================

void route_calc_on_path_add(const route_head_t *head)
{
    update_prefix(head, NULL);
}

void route_calc_on_path_del(const route_head_t *head, const route_path_t *del_path)
{
    if (!del_path)
    {
        return;
    }
    update_prefix(head, &del_path->key);
}

void route_calc_on_periodic(void)
{
    if (!g_calc_os_retry_table || g_hash_table_size(g_calc_os_retry_table) == 0 || !g_route_work_local ||
        !g_route_work_local->rib)
    {
        return;
    }

    const gint64 now = g_get_real_time();
    GHashTableIter iter;
    gpointer key_ptr = NULL;
    gpointer value_ptr = NULL;
    GList *due_keys = NULL;

    g_hash_table_iter_init(&iter, g_calc_os_retry_table);
    while (g_hash_table_iter_next(&iter, &key_ptr, &value_ptr))
    {
        const route_head_key_t *key = (const route_head_key_t *)key_ptr;
        const calc_retry_state_t *state = (const calc_retry_state_t *)value_ptr;
        if (!key || !state || state->due_usec > now)
        {
            continue;
        }

        route_head_key_t *copy = (route_head_key_t *)g_malloc(sizeof(*copy));
        if (!copy)
        {
            continue;
        }
        *copy = *key;
        due_keys = g_list_prepend(due_keys, copy);
    }

    for (GList *l = due_keys; l; l = l->next)
    {
        route_head_key_t *key = (route_head_key_t *)l->data;
        if (!key)
        {
            continue;
        }

        calc_retry_state_t *state = (calc_retry_state_t *)g_hash_table_lookup(g_calc_os_retry_table, key);
        if (!state || state->due_usec > now)
        {
            continue;
        }

        const route_head_t *head =
            route_rib_lookup_head(g_route_work_local->rib, key->vrf_id, key->afi, &key->addr, key->prefix_len);
        if (!head)
        {
            g_hash_table_remove(g_calc_os_retry_table, key);
            continue;
        }

        update_prefix(head, NULL);
    }

    g_list_free_full(due_keys, g_free);
}

void route_calc_schedule_fib_retry(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len)
{
    if (!g_route_work_local || !g_route_work_local->rib || !prefix_addr)
    {
        return;
    }

    const route_head_t *head = route_rib_lookup_head(g_route_work_local->rib, vrf_id, afi, prefix_addr, prefix_len);
    if (!head)
    {
        return;
    }

    schedule_os_install_retry(head);
}

// ============================================================================
// 公共接口：全量最优路径快照（用于 SUBSCRIBE+FULL 响应）
// ============================================================================

typedef struct
{
    route_msg_entry_t *entries; /**< 收集到的条目数组 */
    uint32_t count;             /**< 已收集条目数 */
    uint32_t capacity;          /**< 数组容量 */
    uint16_t afi_filter;        /**< AFI 过滤（ROUTE_AFI_ALL = 全部） */
} dump_collect_ctx_t;

static void dump_collect_best(const route_head_t *head, const route_path_t *path, void *userdata)
{
    dump_collect_ctx_t *dctx = (dump_collect_ctx_t *)userdata;
    if (!head || !path || !dctx)
    {
        return;
    }

    if (!(path->flags & ROUTE_PATH_FLAG_OS_INSTALLED))
    {
        return;
    }
    if (dctx->afi_filter != ROUTE_AFI_ALL && dctx->afi_filter != head->key.afi)
    {
        return;
    }

    if (dctx->count >= dctx->capacity)
    {
        uint32_t new_cap = dctx->capacity ? dctx->capacity * 2u : 64u;
        route_msg_entry_t *new_buf = (route_msg_entry_t *)g_realloc(dctx->entries, new_cap * sizeof(route_msg_entry_t));
        if (!new_buf)
        {
            return;
        }
        dctx->entries = new_buf;
        dctx->capacity = new_cap;
    }

    build_report_entry(&dctx->entries[dctx->count], head, path);
    dctx->entries[dctx->count].is_withdraw = 0;
    dctx->count++;
}

void route_calc_pub_dump(uint32_t dst_module_id, uint32_t protocol, uint32_t vrf_id, uint16_t afi, uint32_t request_id)
{
    if (!g_route_work_local || !g_route_work_local->rib)
    {
        return;
    }
    dev_ipc_context_t *pub_ctx = route_local_ipc_ctx();

    dump_collect_ctx_t dctx = {
        .entries = NULL,
        .count = 0,
        .capacity = 0,
        .afi_filter = afi,
    };
    route_rib_walk(g_route_work_local->rib, protocol, vrf_id, dump_collect_best, &dctx);

    size_t report_size = sizeof(route_msg_report_t) + dctx.count * sizeof(route_msg_entry_t);
    route_msg_report_t *report = (route_msg_report_t *)g_malloc(report_size);
    if (!report)
    {
        g_free(dctx.entries);
        return;
    }

    report->protocol = protocol;
    report->route_count = dctx.count;
    if (dctx.count > 0)
    {
        memcpy(report->routes, dctx.entries, dctx.count * sizeof(route_msg_entry_t));
    }
    g_free(dctx.entries);

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_REPORT, DEV_MODULE_ID_ROUTE, dst_module_id,
                                                    request_id, report, (uint32_t)report_size, g_free);
    if (!msg)
    {
        g_free(report);
        return;
    }

    if (dev_ipc_send_response(pub_ctx, msg) != ERRCODE_SUCCESS)
    {
        LOG_WARN("[route_calc] 全量快照发送失败 -> module 0x%08X", dst_module_id);
    }
    dev_ipc_message_free(msg);

    LOG_INFO("[route_calc] 全量最优路径快照: %u 条 -> module 0x%08X protocol=%u vrf=%u afi=%u", dctx.count,
             dst_module_id, protocol, vrf_id, afi);
}
