/**
 * @file   route_static.c
 * @brief  静态路由候选表：存储配置的静态路由，仅在 nexthop 迭代可达时才写入 RIB
 * @author jhb
 * @date   2026-03-21
 */
#include "route_static.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_calc.h"
#include "route_main.h"
#include "route_pub.h"
#include "route_relay.h"
#include "route_rib.h"

/* 候选静态路由哈希表：route_static_entry_key_t* -> route_static_entry_t*（key 内嵌于 value） */
static GHashTable *g_static_table = NULL;

// ============================================================================
// 哈希 / 相等函数
// ============================================================================

static guint static_key_hash(gconstpointer p)
{
    const route_static_entry_key_t *k = (const route_static_entry_key_t *)p;
    if (!k)
    {
        return 0;
    }

    guint h = (guint)k->vrf_id;
    h = h * 33u + (guint)k->afi;
    h = h * 33u + (guint)k->prefix_len;
    h ^= net_addr_hash(&k->prefix_addr);
    h ^= net_addr_hash(&k->nexthop_addr) * 31u;
    return h;
}

static gboolean static_key_equal(gconstpointer a, gconstpointer b)
{
    const route_static_entry_key_t *ka = (const route_static_entry_key_t *)a;
    const route_static_entry_key_t *kb = (const route_static_entry_key_t *)b;
    if (!ka || !kb)
    {
        return FALSE;
    }

    return ka->vrf_id == kb->vrf_id && ka->afi == kb->afi && ka->prefix_len == kb->prefix_len &&
           net_addr_equal(&ka->prefix_addr, &kb->prefix_addr) && net_addr_equal(&ka->nexthop_addr, &kb->nexthop_addr);
}

// ============================================================================
// 辅助：撤销 RIB 条目时的通知回调
// ============================================================================

static void on_static_rib_del(const route_head_t *head, const route_path_t *path, void *userdata)
{
    (void)userdata;
    if (!g_route_local)
    {
        return;
    }

    /* 触发优选重算：路径仍在 RIB 中，calc 跳过此路径选次优并同步 OS */
    route_calc_on_path_del(head, path);

    route_pub_notify(g_route_local->subscribers, head, path, 1);
}

// ============================================================================
// 初始化 / 清理
// ============================================================================

void route_static_init(void)
{
    if (g_static_table)
    {
        return;
    }
    g_static_table = g_hash_table_new_full(static_key_hash, static_key_equal, NULL, g_free);
}

void route_static_cleanup(void)
{
    if (!g_static_table)
    {
        return;
    }
    g_hash_table_destroy(g_static_table);
    g_static_table = NULL;
}

// ============================================================================
// show route static
// ============================================================================

typedef struct
{
    GString *buf;
    uint16_t afi_filter;
    int has_afi_filter;
    uint32_t count;
} static_show_ctx_t;

static void static_show_cb(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    const route_static_entry_t *entry = (const route_static_entry_t *)value_ptr;
    static_show_ctx_t *ctx = (static_show_ctx_t *)user_data;
    if (!entry || !ctx)
    {
        return;
    }

    /* 按 AFI 过滤 */
    if (ctx->has_afi_filter && entry->key.afi != ctx->afi_filter)
    {
        return;
    }

    char prefix_str[64];
    char nh_str[64];
    char prefix_with_len[80];

    net_addr_to_str(&entry->key.prefix_addr, prefix_str, sizeof(prefix_str));
    net_addr_to_str(&entry->key.nexthop_addr, nh_str, sizeof(nh_str));
    snprintf(prefix_with_len, sizeof(prefix_with_len), "%s/%u", prefix_str, (unsigned)entry->key.prefix_len);

    const char *afi_str = (entry->key.afi == ROUTE_AFI_IPV4) ? "ipv4" : "ipv6";
    const char *resolved_str = entry->nh_resolved ? "yes" : "no ";
    const char *in_rib_str = entry->in_rib ? "yes" : "no ";

    g_string_append_printf(ctx->buf, "%-4s %-24s %-20s %4d %4d  %-8s  %s\r\n", afi_str, prefix_with_len, nh_str,
                           entry->metric, entry->preference, resolved_str, in_rib_str);
    ctx->count++;
}

void route_static_show(GString *buf, uint16_t afi_filter, int has_afi_filter)
{
    if (!buf)
    {
        return;
    }

    g_string_append_printf(buf,
                           "\r\n%-4s %-24s %-20s %4s %4s  %-8s  %s\r\n"
                           "---- ------------------------ -------------------- ---- ----  --------  ------\r\n",
                           "AFI", "Prefix", "Nexthop", "Met", "Pref", "Resolved", "InRIB");

    if (!g_static_table || g_hash_table_size(g_static_table) == 0)
    {
        g_string_append(buf, "  (no entries)\r\n");
        g_string_append(buf, "\r\nTotal 0 static route(s)\r\n");
        return;
    }

    static_show_ctx_t ctx = {.buf = buf, .afi_filter = afi_filter, .has_afi_filter = has_afi_filter, .count = 0};
    g_hash_table_foreach(g_static_table, static_show_cb, &ctx);

    g_string_append_printf(buf, "\r\nTotal %u static route(s) in candidate table\r\n", ctx.count);
}

// ============================================================================
// 添加候选静态路由
// ============================================================================

int route_static_add(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                     const net_addr_t *nexthop_addr, int32_t metric, int32_t preference)
{
    if (!prefix_addr || !nexthop_addr || !g_static_table || !g_route_local)
    {
        return -1;
    }

    /* 构造查找键 */
    route_static_entry_key_t key;
    memset(&key, 0, sizeof(key));
    key.vrf_id = vrf_id;
    key.afi = afi;
    key.prefix_len = prefix_len;
    key.prefix_addr = *prefix_addr;
    key.nexthop_addr = *nexthop_addr;

    /* 查找已有条目（upsert） */
    route_static_entry_t *entry = (route_static_entry_t *)g_hash_table_lookup(g_static_table, &key);
    gboolean is_new = (entry == NULL);

    if (is_new)
    {
        entry = (route_static_entry_t *)g_malloc0(sizeof(route_static_entry_t));
        if (!entry)
        {
            return -1;
        }
        entry->key = key;
        g_hash_table_insert(g_static_table, &entry->key, entry);
    }

    /* 更新属性 */
    entry->metric = metric;
    entry->preference = preference;

    /* 向通用 relay watch 表注册 nexthop，同时获取当前可达性 */
    int resolved = route_relay_register_direct(vrf_id, afi, nexthop_addr, DEV_MODULE_ID_ROUTE);
    entry->nh_resolved = resolved ? 1u : 0u;

    if (resolved)
    {
        /* nexthop 可达：写入或更新 RIB */
        uint32_t oif = route_relay_nh_get_ifindex(vrf_id, afi, nexthop_addr);
        int ret = route_add_and_notify(vrf_id, afi, prefix_addr, prefix_len, ROUTE_PROTOCOL_STATIC, nexthop_addr,
                                       nexthop_addr, metric, preference, oif);
        if (ret >= 0)
        {
            entry->in_rib = 1u;
            /* OS 下发由 route_add_and_notify 内部调用 route_calc_on_path_add 完成 */
            LOG_DEBUG("Static route written to RIB: vrf=%u afi=%u pfxlen=%u (nexthop resolved)", vrf_id, afi,
                      prefix_len);
        }
    }
    else
    {
        if (entry->in_rib)
        {
            /* nexthop 变不可达：从 RIB 撤销 */
            route_rib_del(g_route_local->rib, vrf_id, afi, prefix_addr, prefix_len, ROUTE_PROTOCOL_STATIC, nexthop_addr,
                          on_static_rib_del, NULL);
            entry->in_rib = 0u;
            LOG_DEBUG("Static route withdrawn from RIB: vrf=%u afi=%u pfxlen=%u (nexthop unresolved)", vrf_id, afi,
                      prefix_len);
        }
        else
        {
            char nh_str[64];
            net_addr_to_str(nexthop_addr, nh_str, sizeof(nh_str));
            LOG_DEBUG("Static route held in candidate table (nexthop %s unresolved): vrf=%u afi=%u pfxlen=%u", nh_str,
                      vrf_id, afi, prefix_len);
        }
    }

    return 0;
}

// ============================================================================
// 删除候选静态路由（精确匹配 prefix + nexthop）
// ============================================================================

/**
 * @brief 检查候选表中是否还有其他条目（不含 exclude_key 自身）使用相同的 (vrf, afi, nexthop)
 */
static int static_nexthop_has_users(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr)
{
    if (!g_static_table)
    {
        return 0;
    }
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, g_static_table);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const route_static_entry_t *e = (const route_static_entry_t *)value;
        if (e->key.vrf_id == vrf_id && e->key.afi == afi && net_addr_equal(&e->key.nexthop_addr, nexthop_addr))
        {
            return 1;
        }
    }
    return 0;
}

int route_static_del(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                     const net_addr_t *nexthop_addr)
{
    if (!prefix_addr || !nexthop_addr || !g_static_table || !g_route_local)
    {
        return -1;
    }

    route_static_entry_key_t key;
    memset(&key, 0, sizeof(key));
    key.vrf_id = vrf_id;
    key.afi = afi;
    key.prefix_len = prefix_len;
    key.prefix_addr = *prefix_addr;
    key.nexthop_addr = *nexthop_addr;

    route_static_entry_t *entry = (route_static_entry_t *)g_hash_table_lookup(g_static_table, &key);
    if (!entry)
    {
        return 0;
    }

    /* 若已在 RIB 中则先撤销并通知订阅者 */
    if (entry->in_rib)
    {
        route_rib_del(g_route_local->rib, vrf_id, afi, prefix_addr, prefix_len, ROUTE_PROTOCOL_STATIC, nexthop_addr,
                      on_static_rib_del, NULL);
    }

    /* 从候选表移除；移除后再判断该 nexthop 是否还有其他引用者 */
    g_hash_table_remove(g_static_table, &key);

    if (!static_nexthop_has_users(vrf_id, afi, nexthop_addr))
    {
        route_relay_unregister_direct(vrf_id, afi, nexthop_addr, DEV_MODULE_ID_ROUTE);
    }

    return 1;
}

// ============================================================================
// 删除某前缀下所有候选静态路由
// ============================================================================

typedef struct
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t prefix_len;
    uint8_t _pad;
    const net_addr_t *prefix_addr;
    GSList *keys_to_remove;   /**< 待从候选表移除的键指针列表（指向 entry->key） */
    GSList *nexthops_removed; /**< 被删除条目的 nexthop 拷贝列表（需在移除后检查是否孤立） */
    int count;
} static_del_prefix_ctx_t;

static void static_collect_for_prefix(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    route_static_entry_t *entry = (route_static_entry_t *)value_ptr;
    static_del_prefix_ctx_t *pctx = (static_del_prefix_ctx_t *)user_data;
    if (!entry || !pctx)
    {
        return;
    }

    if (entry->key.vrf_id != pctx->vrf_id || entry->key.afi != pctx->afi || entry->key.prefix_len != pctx->prefix_len ||
        !net_addr_equal(&entry->key.prefix_addr, pctx->prefix_addr))
    {
        return;
    }

    /* 若已在 RIB 中则先撤销（在 foreach 中修改 rib 是安全的） */
    if (entry->in_rib)
    {
        route_rib_del(g_route_local->rib, entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                      entry->key.prefix_len, ROUTE_PROTOCOL_STATIC, &entry->key.nexthop_addr, on_static_rib_del, NULL);
    }

    /* 拷贝 nexthop 地址，用于移除后检查是否孤立 */
    net_addr_t *nh_copy = (net_addr_t *)g_malloc(sizeof(net_addr_t));
    if (nh_copy)
    {
        *nh_copy = entry->key.nexthop_addr;
        pctx->nexthops_removed = g_slist_prepend(pctx->nexthops_removed, nh_copy);
    }

    /* 收集键用于后续从候选表中移除（不能在 foreach 中修改 g_static_table） */
    pctx->keys_to_remove = g_slist_prepend(pctx->keys_to_remove, &entry->key);
    pctx->count++;
}

int route_static_del_prefix(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len)
{
    if (!prefix_addr || !g_static_table || !g_route_local)
    {
        return -1;
    }

    static_del_prefix_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.vrf_id = vrf_id;
    pctx.afi = afi;
    pctx.prefix_len = prefix_len;
    pctx.prefix_addr = prefix_addr;

    /* 第一轮：收集匹配条目并撤销 RIB */
    g_hash_table_foreach(g_static_table, static_collect_for_prefix, &pctx);

    /* 第二轮：从候选表移除 */
    for (GSList *l = pctx.keys_to_remove; l; l = l->next)
    {
        g_hash_table_remove(g_static_table, l->data);
    }
    g_slist_free(pctx.keys_to_remove);

    /* 第三轮：对已移除条目的 nexthop，若无其他引用则注销 relay watch */
    for (GSList *l = pctx.nexthops_removed; l; l = l->next)
    {
        net_addr_t *nh = (net_addr_t *)l->data;
        if (!static_nexthop_has_users(vrf_id, afi, nh))
        {
            route_relay_unregister_direct(vrf_id, afi, nh, DEV_MODULE_ID_ROUTE);
        }
        g_free(nh);
    }
    g_slist_free(pctx.nexthops_removed);

    return pctx.count;
}

// ============================================================================
// 全量重算候选静态路由的 nexthop 可达性
// ============================================================================

typedef struct
{
    uint32_t total;
    uint32_t added;
    uint32_t withdrawn;
} static_recompute_ctx_t;

static void static_recompute_cb(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    route_static_entry_t *entry = (route_static_entry_t *)value_ptr;
    static_recompute_ctx_t *rctx = (static_recompute_ctx_t *)user_data;
    if (!entry || !rctx || !g_route_local)
    {
        return;
    }

    rctx->total++;

    int resolved = route_relay_nh_is_resolved(entry->key.vrf_id, entry->key.afi, &entry->key.nexthop_addr);
    uint8_t new_resolved = resolved ? 1u : 0u;

    /* 状态与 RIB 均一致时跳过 */
    if (new_resolved == entry->nh_resolved && entry->in_rib == new_resolved)
    {
        return;
    }

    entry->nh_resolved = new_resolved;

    if (resolved && !entry->in_rib)
    {
        /* nexthop 新可达：写入 RIB */
        uint32_t oif = route_relay_nh_get_ifindex(entry->key.vrf_id, entry->key.afi, &entry->key.nexthop_addr);
        int ret = route_add_and_notify(entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                                       entry->key.prefix_len, ROUTE_PROTOCOL_STATIC, &entry->key.nexthop_addr,
                                       &entry->key.nexthop_addr, entry->metric, entry->preference, oif);
        if (ret >= 0)
        {
            entry->in_rib = 1U;
            /* OS 下发由 route_add_and_notify 内部调用 route_calc_on_path_add 完成 */
            rctx->added++;
        }
    }
    else if (!resolved && entry->in_rib)
    {
        /* nexthop 变不可达：从 RIB 撤销 */
        route_rib_del(g_route_local->rib, entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                      entry->key.prefix_len, ROUTE_PROTOCOL_STATIC, &entry->key.nexthop_addr, on_static_rib_del, NULL);
        entry->in_rib = 0u;
        rctx->withdrawn++;
    }
}

void route_static_recompute(void)
{
    if (!g_static_table || g_hash_table_size(g_static_table) == 0)
    {
        return;
    }

    static_recompute_ctx_t rctx = {
        .total = 0u,
        .added = 0u,
        .withdrawn = 0u,
    };

    g_hash_table_foreach(g_static_table, static_recompute_cb, &rctx);

    if (rctx.added > 0 || rctx.withdrawn > 0)
    {
        LOG_DEBUG("Static route recompute: total=%u added=%u withdrawn=%u", rctx.total, rctx.added, rctx.withdrawn);
    }
}

// ============================================================================
// show route relay static（直接委托通用 relay show，按 owner_module_id 过滤）
// ============================================================================

void route_static_show_relay(GString *buf, uint16_t afi_filter, int has_afi_filter)
{
    if (!buf)
    {
        return;
    }
    route_relay_show(buf, DEV_MODULE_ID_ROUTE, 1, afi_filter, has_afi_filter);
}
