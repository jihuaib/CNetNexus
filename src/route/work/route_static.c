/**
 * @file   route_static.c
 * @brief  静态路由候选表：存储配置的静态路由，仅在 nexthop 迭代可达时才写入 RIB
 * @author jhb
 * @date   2026-03-21
 */
#include "route_static.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "if.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_calc.h"
#include "route_main.h"
#include "route_pub.h"
#include "route_relay.h"
#include "route_rib.h"
#include "route_worker.h"

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
    /* out_ifname 参与 hash */
    for (const char *c = k->out_ifname; *c; c++)
    {
        h = h * 33u + (guint)(unsigned char)*c;
    }
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
           net_addr_equal(&ka->prefix_addr, &kb->prefix_addr) && net_addr_equal(&ka->nexthop_addr, &kb->nexthop_addr) &&
           strcmp(ka->out_ifname, kb->out_ifname) == 0;
}

// ============================================================================
// 辅助：撤销 RIB 条目时的通知回调
// ============================================================================

static void on_static_rib_del(const route_head_t *head, const route_path_t *path, void *userdata)
{
    (void)userdata;
    if (!g_route_work_local)
    {
        return;
    }

    /* 触发优选重算：路径仍在 RIB 中，calc 跳过此路径选次优并同步 OS */
    route_calc_on_path_del(head, path);

    route_pub_notify(g_route_work_local->subscribers, head, path, 1);
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
    uint32_t vrf_filter;
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
    if (entry->key.vrf_id != ctx->vrf_filter)
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
    const char *ifname = entry->key.out_ifname[0] ? entry->key.out_ifname : "-";

    g_string_append_printf(ctx->buf, "%-4s %-24s %-20s %-10s %4d %4d  %-8s  %s\r\n", afi_str, prefix_with_len, nh_str,
                           ifname, entry->metric, entry->preference, resolved_str, in_rib_str);
    ctx->count++;
}

void route_static_show(GString *buf, uint16_t afi_filter, int has_afi_filter, uint32_t vrf_filter, const char *vrf_name)
{
    if (!buf)
    {
        return;
    }

    g_string_append_printf(buf, "\r\nStatic Routes (VRF: %s)\r\n", vrf_name ? vrf_name : "public");
    g_string_append_printf(
        buf,
        "\r\n%-4s %-24s %-20s %-10s %4s %4s  %-8s  %s\r\n"
        "---- ------------------------ -------------------- ---------- ---- ----  --------  ------\r\n",
        "AFI", "Prefix", "Nexthop", "Interface", "Met", "Pref", "Resolved", "InRIB");

    if (!g_static_table || g_hash_table_size(g_static_table) == 0)
    {
        g_string_append(buf, "  (no entries)\r\n");
        g_string_append(buf, "\r\nTotal 0 static route(s)\r\n");
        return;
    }

    static_show_ctx_t ctx = {
        .buf = buf,
        .afi_filter = afi_filter,
        .has_afi_filter = has_afi_filter,
        .vrf_filter = vrf_filter,
        .count = 0,
    };
    g_hash_table_foreach(g_static_table, static_show_cb, &ctx);

    g_string_append_printf(buf, "\r\nTotal %u static route(s) in candidate table\r\n", ctx.count);
}

// ============================================================================
// 辅助：接口名 → ifindex 解析
// ============================================================================

static uint32_t resolve_ifindex(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return 0;
    }
    return if_api_cache_get_ifindex(ifname);
}

// ============================================================================
// 辅助：检查 RIB 中是否有指定 ifindex 的已安装 CONNECTED 路由
// ============================================================================

typedef struct
{
    uint16_t afi;
    uint32_t target_ifindex;
    int found;
} connected_check_ctx_t;

static void connected_check_cb(const route_head_t *head, const route_path_t *path, void *userdata)
{
    connected_check_ctx_t *ctx = (connected_check_ctx_t *)userdata;
    if (ctx->found || !head || !path)
    {
        return;
    }
    /* 按 AFI 过滤（walk 只按协议和 VRF 过滤） */
    if (head->key.afi != ctx->afi)
    {
        return;
    }
    if (path->out_ifindex == ctx->target_ifindex && (path->flags & ROUTE_PATH_FLAG_OS_INSTALLED))
    {
        ctx->found = 1;
    }
}

static int rib_has_active_connected(uint32_t vrf_id, uint16_t afi, uint32_t ifindex)
{
    if (!g_route_work_local || !g_route_work_local->rib || ifindex == 0)
    {
        return 0;
    }
    connected_check_ctx_t ctx = {.afi = afi, .target_ifindex = ifindex, .found = 0};
    route_rib_walk(g_route_work_local->rib, ROUTE_PROTOCOL_CONNECTED, vrf_id, connected_check_cb, &ctx);
    return ctx.found;
}

/**
 * @brief 将 ifindex 编码为 net_addr_t（用于 interface-only 路由的 RIB path source）
 */
static void encode_ifindex_as_addr(uint32_t ifindex, net_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    out->family = AF_INET;
    out->u.v4.s_addr = htonl(ifindex);
}

// ============================================================================
// 添加候选静态路由
// ============================================================================

int route_static_add(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                     const net_addr_t *nexthop_addr, int32_t metric, int32_t preference, const char *out_ifname)
{
    if (!prefix_addr || !nexthop_addr || !g_static_table || !g_route_work_local)
    {
        return -1;
    }

    const char *safe_ifname = (out_ifname && out_ifname[0]) ? out_ifname : "";
    int has_nh = !net_addr_is_zero(nexthop_addr);

    /* 构造查找键 */
    route_static_entry_key_t key;
    memset(&key, 0, sizeof(key));
    key.vrf_id = vrf_id;
    key.afi = afi;
    key.prefix_len = prefix_len;
    key.prefix_addr = *prefix_addr;
    key.nexthop_addr = *nexthop_addr;
    g_strlcpy(key.out_ifname, safe_ifname, sizeof(key.out_ifname));

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
        entry->has_nexthop = has_nh ? 1u : 0u;
        g_hash_table_insert(g_static_table, &entry->key, entry);
    }

    /* 更新属性 */
    entry->metric = metric;
    entry->preference = preference;

    if (has_nh)
    {
        /* 模式 A（nexthop + interface）或纯 nexthop：复用 relay 迭代 */
        net_addr_t relay_addr;
        uint32_t oif = 0;
        memset(&relay_addr, 0, sizeof(relay_addr));
        int resolved = route_relay_register_direct(vrf_id, afi, nexthop_addr, DEV_MODULE_ID_ROUTE, &relay_addr, &oif);

        /* 模式 A：若指定了接口，校验 relay 解析出的 ifindex 是否匹配 */
        if (resolved && safe_ifname[0] != '\0')
        {
            uint32_t cfg_ifidx = resolve_ifindex(safe_ifname);
            entry->cfg_ifindex = cfg_ifidx;
            if (cfg_ifidx != 0 && oif != cfg_ifidx)
            {
                resolved = 0; /* ifindex 不匹配 → 视为不可达 */
            }
        }

        entry->nh_resolved = resolved ? 1u : 0u;

        if (resolved)
        {
            int ret = route_add_and_notify(vrf_id, afi, prefix_addr, prefix_len, ROUTE_PROTOCOL_STATIC, nexthop_addr,
                                           nexthop_addr, &relay_addr, metric, preference, oif);
            if (ret >= 0)
            {
                entry->in_rib = 1u;
                LOG_DEBUG("Static route written to RIB: vrf=%u afi=%u pfxlen=%u (nexthop resolved)", vrf_id, afi,
                          prefix_len);
            }
        }
        else
        {
            if (entry->in_rib)
            {
                route_rib_del(g_route_work_local->rib, vrf_id, afi, prefix_addr, prefix_len, ROUTE_PROTOCOL_STATIC,
                              nexthop_addr, on_static_rib_del, NULL);
                entry->in_rib = 0u;
            }
            LOG_DEBUG("Static route held in candidate table (nexthop unresolved): vrf=%u afi=%u pfxlen=%u", vrf_id, afi,
                      prefix_len);
        }
    }
    else
    {
        /* interface-only 路由 */
        if (g_ascii_strcasecmp(safe_ifname, "null0") == 0)
        {
            /* 模式 C：null0 黑洞路由，配置即生效 */
            net_addr_t zero_src;
            memset(&zero_src, 0, sizeof(zero_src));
            zero_src.family = (afi == ROUTE_AFI_IPV6) ? AF_INET6 : AF_INET;
            net_addr_t zero_nh = zero_src;

            int ret = route_add_and_notify(vrf_id, afi, prefix_addr, prefix_len, ROUTE_PROTOCOL_BLACKHOLE, &zero_src,
                                           &zero_nh, NULL, metric, preference, 0);
            if (ret >= 0)
            {
                entry->in_rib = 1u;
                entry->nh_resolved = 1u;
                LOG_DEBUG("null0 blackhole route written to RIB: vrf=%u afi=%u pfxlen=%u", vrf_id, afi, prefix_len);
            }
        }
        else
        {
            /* 模式 B：普通接口，检查 RIB 中是否有活跃 CONNECTED 路由 */
            uint32_t cfg_ifidx = resolve_ifindex(safe_ifname);
            entry->cfg_ifindex = cfg_ifidx;

            int reachable = (cfg_ifidx != 0) && rib_has_active_connected(vrf_id, afi, cfg_ifidx);
            entry->nh_resolved = reachable ? 1u : 0u;

            if (reachable)
            {
                net_addr_t source_addr;
                encode_ifindex_as_addr(cfg_ifidx, &source_addr);
                net_addr_t zero_nh;
                memset(&zero_nh, 0, sizeof(zero_nh));
                zero_nh.family = (afi == ROUTE_AFI_IPV6) ? AF_INET6 : AF_INET;

                int ret = route_add_and_notify(vrf_id, afi, prefix_addr, prefix_len, ROUTE_PROTOCOL_STATIC,
                                               &source_addr, &zero_nh, NULL, metric, preference, cfg_ifidx);
                if (ret >= 0)
                {
                    entry->in_rib = 1u;
                    LOG_DEBUG("Interface-only static route written to RIB: vrf=%u afi=%u pfxlen=%u ifname=%s", vrf_id,
                              afi, prefix_len, safe_ifname);
                }
            }
            else
            {
                if (entry->in_rib)
                {
                    net_addr_t source_addr;
                    encode_ifindex_as_addr(entry->cfg_ifindex, &source_addr);
                    route_rib_del(g_route_work_local->rib, vrf_id, afi, prefix_addr, prefix_len, ROUTE_PROTOCOL_STATIC,
                                  &source_addr, on_static_rib_del, NULL);
                    entry->in_rib = 0u;
                }
                LOG_DEBUG("Interface-only static route held in candidate table (if=%s): vrf=%u afi=%u pfxlen=%u",
                          safe_ifname, vrf_id, afi, prefix_len);
            }
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
                     const net_addr_t *nexthop_addr, const char *out_ifname)
{
    if (!prefix_addr || !nexthop_addr || !g_static_table || !g_route_work_local)
    {
        return -1;
    }

    const char *safe_ifname = (out_ifname && out_ifname[0]) ? out_ifname : "";

    route_static_entry_key_t key;
    memset(&key, 0, sizeof(key));
    key.vrf_id = vrf_id;
    key.afi = afi;
    key.prefix_len = prefix_len;
    key.prefix_addr = *prefix_addr;
    key.nexthop_addr = *nexthop_addr;
    g_strlcpy(key.out_ifname, safe_ifname, sizeof(key.out_ifname));

    route_static_entry_t *entry = (route_static_entry_t *)g_hash_table_lookup(g_static_table, &key);
    if (!entry)
    {
        return 0;
    }

    /* g_hash_table_remove() will free entry (value destroy func = g_free), cache fields needed afterwards. */
    uint8_t had_nexthop = entry->has_nexthop;
    net_addr_t del_nexthop = entry->key.nexthop_addr;

    /* 确定 RIB 中的 source 地址 */
    const net_addr_t *rib_source = nexthop_addr;
    net_addr_t if_source;
    int is_null0 = (!entry->has_nexthop && g_ascii_strcasecmp(safe_ifname, "null0") == 0);
    if (!entry->has_nexthop && !is_null0)
    {
        /* interface-only：source 是 ifindex 编码 */
        encode_ifindex_as_addr(entry->cfg_ifindex, &if_source);
        rib_source = &if_source;
    }
    else if (is_null0)
    {
        /* null0 黑洞：source 是全零地址 */
        net_addr_t zero_src;
        memset(&zero_src, 0, sizeof(zero_src));
        zero_src.family = (afi == ROUTE_AFI_IPV6) ? AF_INET6 : AF_INET;
        if_source = zero_src;
        rib_source = &if_source;
    }

    /* 若已在 RIB 中则先撤销并通知订阅者 */
    if (entry->in_rib)
    {
        uint32_t protocol = is_null0 ? ROUTE_PROTOCOL_BLACKHOLE : ROUTE_PROTOCOL_STATIC;
        route_rib_del(g_route_work_local->rib, vrf_id, afi, prefix_addr, prefix_len, protocol, rib_source,
                      on_static_rib_del, NULL);
    }

    /* 从候选表移除；对有 nexthop 的路由判断是否还有其他引用者 */
    g_hash_table_remove(g_static_table, &key);

    if (had_nexthop && !static_nexthop_has_users(vrf_id, afi, &del_nexthop))
    {
        route_relay_unregister_direct(vrf_id, afi, &del_nexthop, DEV_MODULE_ID_ROUTE);
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
        route_rib_del(g_route_work_local->rib, entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
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
    if (!prefix_addr || !g_static_table || !g_route_work_local)
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
// nexthop 状态变化回调（由 relay 统一触发，与协议迭代同一流程）
// ============================================================================

typedef struct
{
    uint32_t vrf_id;
    uint16_t afi;
    const net_addr_t *nexthop_addr;
    int resolved;
    const net_addr_t *gateway; /**< relay 已解析的直连网关 */
    uint32_t out_ifindex;      /**< relay 已解析的出接口 */
    uint32_t added;
    uint32_t withdrawn;
} static_nh_change_ctx_t;

static void static_nh_change_cb(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    route_static_entry_t *entry = (route_static_entry_t *)value_ptr;
    static_nh_change_ctx_t *ctx = (static_nh_change_ctx_t *)user_data;
    if (!entry || !ctx || !g_route_work_local)
    {
        return;
    }

    /* 仅处理有 nexthop 的条目（interface-only 由 on_if_change 处理） */
    if (!entry->has_nexthop)
    {
        return;
    }

    /* 只处理匹配 (vrf, afi, nexthop) 的条目 */
    if (entry->key.vrf_id != ctx->vrf_id || entry->key.afi != ctx->afi ||
        !net_addr_equal(&entry->key.nexthop_addr, ctx->nexthop_addr))
    {
        return;
    }

    int resolved = ctx->resolved;
    uint32_t oif = ctx->out_ifindex;

    /* 模式 A：若指定了接口，校验 relay 解析出的 ifindex 是否匹配 */
    if (resolved && entry->key.out_ifname[0] != '\0')
    {
        if (entry->cfg_ifindex != 0 && oif != entry->cfg_ifindex)
        {
            resolved = 0;
        }
    }

    entry->nh_resolved = resolved ? 1u : 0u;

    if (resolved)
    {
        int ret = route_add_and_notify(entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                                       entry->key.prefix_len, ROUTE_PROTOCOL_STATIC, &entry->key.nexthop_addr,
                                       &entry->key.nexthop_addr, ctx->gateway, entry->metric, entry->preference, oif);
        if (ret >= 0 && !entry->in_rib)
        {
            entry->in_rib = 1u;
            ctx->added++;
        }
    }
    else if (entry->in_rib)
    {
        route_rib_del(g_route_work_local->rib, entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                      entry->key.prefix_len, ROUTE_PROTOCOL_STATIC, &entry->key.nexthop_addr, on_static_rib_del, NULL);
        entry->in_rib = 0u;
        ctx->withdrawn++;
    }
}

void route_static_on_nh_change(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr, int resolved,
                               const net_addr_t *gateway, uint32_t out_ifindex)
{
    if (!g_static_table || g_hash_table_size(g_static_table) == 0 || !nexthop_addr)
    {
        return;
    }

    static_nh_change_ctx_t ctx = {
        .vrf_id = vrf_id,
        .afi = afi,
        .nexthop_addr = nexthop_addr,
        .resolved = resolved,
        .gateway = gateway,
        .out_ifindex = out_ifindex,
        .added = 0u,
        .withdrawn = 0u,
    };

    g_hash_table_foreach(g_static_table, static_nh_change_cb, &ctx);

    if (ctx.added > 0 || ctx.withdrawn > 0)
    {
        LOG_DEBUG("Static nh-change: vrf=%u afi=%u resolved=%d added=%u withdrawn=%u", vrf_id, afi, resolved, ctx.added,
                  ctx.withdrawn);
    }
}

// ============================================================================
// show route relay static（直接委托通用 relay show，按 owner_module_id 过滤）
// ============================================================================

void route_static_show_relay(GString *buf, uint16_t afi_filter, int has_afi_filter, uint32_t vrf_filter,
                             const char *vrf_name)
{
    if (!buf)
    {
        return;
    }
    route_relay_show(buf, DEV_MODULE_ID_ROUTE, 1, afi_filter, has_afi_filter, vrf_filter, vrf_name);
}

// ============================================================================
// 接口状态变化：重检查 interface-only 静态路由
// ============================================================================

static void static_if_change_cb(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    (void)user_data;
    route_static_entry_t *entry = (route_static_entry_t *)value_ptr;
    if (!entry || !g_route_work_local)
    {
        return;
    }

    /* 仅处理 interface-only 条目（has_nexthop=0） */
    if (entry->has_nexthop)
    {
        return;
    }

    /* null0 黑洞路由永远有效，跳过 */
    if (g_ascii_strcasecmp(entry->key.out_ifname, "null0") == 0)
    {
        return;
    }

    /*
     * IF 事件回调路径避免同步 RPC：优先使用事件驱动缓存，
     * 若缓存暂未命中则回退到条目已有 ifindex。
     */
    uint32_t cached_ifindex = if_api_cache_get_ifindex(entry->key.out_ifname);
    uint32_t ifindex = (cached_ifindex != 0u) ? cached_ifindex : entry->cfg_ifindex;
    if (cached_ifindex != 0u)
    {
        entry->cfg_ifindex = cached_ifindex;
    }

    /* 检查 RIB 中是否有该接口的活跃 CONNECTED 路由 */
    int reachable = (ifindex != 0) && rib_has_active_connected(entry->key.vrf_id, entry->key.afi, ifindex);

    if (reachable && !entry->in_rib)
    {
        /* 接口就绪 → 写入 RIB */
        net_addr_t source_addr;
        encode_ifindex_as_addr(ifindex, &source_addr);
        net_addr_t zero_nh;
        memset(&zero_nh, 0, sizeof(zero_nh));
        zero_nh.family = (entry->key.afi == ROUTE_AFI_IPV6) ? AF_INET6 : AF_INET;

        int ret = route_add_and_notify(entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                                       entry->key.prefix_len, ROUTE_PROTOCOL_STATIC, &source_addr, &zero_nh, NULL,
                                       entry->metric, entry->preference, ifindex);
        if (ret >= 0)
        {
            entry->in_rib = 1u;
            entry->nh_resolved = 1u;
            LOG_DEBUG("Interface-only static route installed: ifname=%s ifindex=%u", entry->key.out_ifname, ifindex);
        }
    }
    else if (!reachable && entry->in_rib)
    {
        /* 接口不可用 → 从 RIB 撤销 */
        uint32_t withdraw_ifindex = (ifindex != 0u) ? ifindex : entry->cfg_ifindex;
        net_addr_t source_addr;
        encode_ifindex_as_addr(withdraw_ifindex, &source_addr);
        route_rib_del(g_route_work_local->rib, entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                      entry->key.prefix_len, ROUTE_PROTOCOL_STATIC, &source_addr, on_static_rib_del, NULL);
        entry->in_rib = 0u;
        entry->nh_resolved = 0u;
        LOG_DEBUG("Interface-only static route withdrawn: ifname=%s", entry->key.out_ifname);
    }
}

void route_static_on_if_change(void)
{
    if (!g_static_table || g_hash_table_size(g_static_table) == 0)
    {
        return;
    }
    g_hash_table_foreach(g_static_table, static_if_change_cb, NULL);
}
