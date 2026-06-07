/**
 * @file   route_static.c
 * @brief  静态路由候选表：按「下一跳」分组的全局静态 nexthop 哈希 + 仅引用 nexthop_id 的路由表
 * @author jhb
 * @date   2026-03-21
 *
 * 设计（与 RIB 路由的 nexthop 对象统一）：
 * - 全局静态 nexthop 哈希 g_static_nh_table：键 = route_nhobj_key_t（与 RIB 路由一致），
 *   值 = route_static_nh_t（复用 route_nhobj_info_t 承载 key+relay；本地维护，不每次查 route_nhobj）。
 *   建组时 route_nhobj_acquire 申请一个 nexthop_id、注册一次 relay 迭代；多前缀共享之。
 * - 路由候选表 g_static_route_table：键 = 前缀 + nexthop_id（路由只看到 nexthop id）。
 * - 迭代/可达性按「下一跳（组）」驱动：on_nh_change 定位组后整组更新其全部前缀。
 */
#include "route_static.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "errcode.h"
#include "fib.h"
#include "if.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_calc.h"
#include "route_main.h"
#include "route_nhobj.h"
#include "route_pub.h"
#include "route_relay.h"
#include "route_rib.h"
#include "route_worker.h"

/* 全局静态 nexthop 哈希：route_nhobj_key_t*(&nh->key) -> route_static_nh_t* */
static GHashTable *g_static_nh_table = NULL;
/* 按 id 反查：nexthop_id -> route_static_nh_t*（借用，不拥有） */
static GHashTable *g_static_nh_by_id = NULL;
/* 路由候选表：route_static_entry_key_t*(&entry->key) -> route_static_entry_t* */
static GHashTable *g_static_route_table = NULL;

// ============================================================================
// 哈希 / 相等函数
// ============================================================================

static guint static_nh_key_hash(gconstpointer p)
{
    const route_nhobj_key_t *k = (const route_nhobj_key_t *)p;
    if (!k)
    {
        return 0;
    }
    guint h = (guint)k->vrf_id;
    h = h * 33u + (guint)k->protocol;
    h = h * 33u + (guint)k->afi;
    h = h * 33u + (guint)k->nh_type;
    h = h * 33u + (guint)k->key_ifindex;
    h ^= net_addr_hash(&k->nexthop);
    return h;
}

static gboolean static_nh_key_equal(gconstpointer a, gconstpointer b)
{
    return route_nhobj_key_equal((const route_nhobj_key_t *)a, (const route_nhobj_key_t *)b) ? TRUE : FALSE;
}

static guint static_rkey_hash(gconstpointer p)
{
    const route_static_entry_key_t *k = (const route_static_entry_key_t *)p;
    if (!k)
    {
        return 0;
    }
    guint h = (guint)k->vrf_id;
    h = h * 33u + (guint)k->afi;
    h = h * 33u + (guint)k->prefix_len;
    h = h * 33u + (guint)k->nexthop_id;
    h ^= net_addr_hash(&k->prefix_addr);
    return h;
}

static gboolean static_rkey_equal(gconstpointer a, gconstpointer b)
{
    const route_static_entry_key_t *ka = (const route_static_entry_key_t *)a;
    const route_static_entry_key_t *kb = (const route_static_entry_key_t *)b;
    if (!ka || !kb)
    {
        return FALSE;
    }
    return ka->vrf_id == kb->vrf_id && ka->afi == kb->afi && ka->prefix_len == kb->prefix_len &&
           ka->nexthop_id == kb->nexthop_id && net_addr_equal(&ka->prefix_addr, &kb->prefix_addr);
}

static int static_relay_addr_equal(const net_addr_t *a, const net_addr_t *b)
{
    if (net_addr_is_zero(a) && net_addr_is_zero(b))
    {
        return 1;
    }
    return net_addr_equal(a, b) ? 1 : 0;
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
    if (!g_static_nh_table)
    {
        g_static_nh_table = g_hash_table_new_full(static_nh_key_hash, static_nh_key_equal, NULL, g_free);
    }
    if (!g_static_nh_by_id)
    {
        g_static_nh_by_id = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
    }
    if (!g_static_route_table)
    {
        g_static_route_table = g_hash_table_new_full(static_rkey_hash, static_rkey_equal, NULL, g_free);
    }
}

void route_static_cleanup(void)
{
    if (g_static_route_table)
    {
        g_hash_table_destroy(g_static_route_table);
        g_static_route_table = NULL;
    }
    if (g_static_nh_by_id)
    {
        g_hash_table_destroy(g_static_nh_by_id);
        g_static_nh_by_id = NULL;
    }
    if (g_static_nh_table)
    {
        g_hash_table_destroy(g_static_nh_table);
        g_static_nh_table = NULL;
    }
}

// ============================================================================
// 辅助：接口名 → ifindex / 直连可达性 / ifindex 编码
// ============================================================================

static uint32_t resolve_ifindex(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return 0;
    }
    return if_api_cache_get_ifindex(ifname);
}

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

static void encode_ifindex_as_addr(uint32_t ifindex, net_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    out->family = AF_INET;
    out->u.v4.s_addr = htonl(ifindex);
}

static void zero_addr_with_family(net_addr_t *out, uint16_t afi)
{
    memset(out, 0, sizeof(*out));
    out->family = (afi == ROUTE_AFI_IPV6) ? AF_INET6 : AF_INET;
}

// ============================================================================
// nexthop 组：键构造 / 解析 / 增删
// ============================================================================

static void build_nh_key(route_nhobj_key_t *key, uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop,
                         uint32_t cfg_ifindex, int is_null0)
{
    memset(key, 0, sizeof(*key));
    key->vrf_id = vrf_id;
    key->afi = afi;
    key->protocol = ROUTE_PROTOCOL_STATIC;
    key->nh_type = is_null0 ? ROUTE_NH_TYPE_BLACKHOLE : ROUTE_NH_TYPE_IP;
    key->key_ifindex = cfg_ifindex;
    key->nexthop = *nexthop; /* has_nexthop=真实地址；否则 zero-with-family（由调用方构造） */
}

/* 把组本地维护的 relay 结果同步写进 nexthop 对象（FIB 侧 value），供 id 注入后下发 OS。
 * relay 是「两步都用的 value」：static 组与 nexthop 对象各维护一份。 */
static void nh_set_object_relay(const route_static_nh_t *nh)
{
    if (nh->is_null0)
    {
        return; /* 黑洞无 relay */
    }
    const net_addr_t *gw = nh->has_nexthop ? &nh->relay_addr : NULL; /* interface-only 无网关 */
    route_nhobj_set_relay(nh->nexthop_id, gw, nh->relay_ifindex);
}

/* 组首次创建时解析可达性 + 注册一次 relay 迭代（has_nexthop 才注册） */
static void nh_resolve_initial(route_static_nh_t *nh)
{
    uint32_t vrf_id = nh->key.vrf_id;
    uint16_t afi = nh->key.afi;

    if (nh->is_null0)
    {
        nh->resolved = 1u; /* null0 黑洞配置即生效 */
        nh->resolved_nh_type = ROUTE_NH_TYPE_BLACKHOLE;
        return;
    }
    if (!nh->has_nexthop)
    {
        /* interface-only：基于直连路由判断 */
        uint32_t oif = nh->key.key_ifindex;
        nh->resolved = ((oif != 0) && rib_has_active_connected(vrf_id, afi, oif)) ? 1u : 0u;
        nh->resolved_nh_type = ROUTE_NH_TYPE_IP;
        nh->relay_ifindex = oif;
        nh_set_object_relay(nh);
        return;
    }

    /* IP nexthop：注册迭代并取首解析结果 */
    net_addr_t relay;
    uint32_t oif = 0;
    uint8_t resolved_nh_type = ROUTE_NH_TYPE_IP;
    memset(&relay, 0, sizeof(relay));
    int resolved = route_relay_register_direct(nh->nexthop_id, DEV_MODULE_ID_ROUTE, &relay, &oif, &resolved_nh_type);
    /* nexthop+interface：relay 解析出接口须与配置接口一致 */
    if (resolved && nh->key.key_ifindex != 0 && oif != nh->key.key_ifindex)
    {
        resolved = 0;
    }
    nh->resolved = resolved ? 1u : 0u;
    nh->resolved_nh_type = resolved ? resolved_nh_type : ROUTE_NH_TYPE_IP;
    if (resolved)
    {
        nh->relay_addr = relay;
        nh->relay_ifindex = oif;
        nh_set_object_relay(nh);
    }
}

static route_static_nh_t *nh_get_or_create(const route_nhobj_key_t *key, int has_nexthop, int is_null0,
                                           const char *ifname)
{
    route_static_nh_t *nh = (route_static_nh_t *)g_hash_table_lookup(g_static_nh_table, key);
    if (nh)
    {
        return nh;
    }

    uint32_t id = 0;
    if (route_nhobj_acquire(key, 0u, &id) != ERRCODE_SUCCESS || id == 0)
    {
        return NULL;
    }

    nh = (route_static_nh_t *)g_malloc0(sizeof(*nh));
    if (!nh)
    {
        route_nhobj_release(id);
        return NULL;
    }
    nh->key = *key;
    nh->nexthop_id = id;
    nh->refcount = 0u;
    nh->has_nexthop = has_nexthop ? 1u : 0u;
    nh->is_null0 = is_null0 ? 1u : 0u;
    nh->resolved = 0u;
    g_strlcpy(nh->out_ifname, ifname ? ifname : "", sizeof(nh->out_ifname));

    g_hash_table_insert(g_static_nh_table, &nh->key, nh);
    g_hash_table_insert(g_static_nh_by_id, GUINT_TO_POINTER(id), nh);

    nh_resolve_initial(nh);
    return nh;
}

static void nh_unref(route_static_nh_t *nh)
{
    if (!nh)
    {
        return;
    }
    if (nh->refcount > 1u)
    {
        nh->refcount--;
        return;
    }
    nh->refcount = 0u;

    if (nh->has_nexthop)
    {
        route_relay_unregister_direct(nh->nexthop_id, DEV_MODULE_ID_ROUTE);
    }
    route_nhobj_release(nh->nexthop_id);
    g_hash_table_remove(g_static_nh_by_id, GUINT_TO_POINTER(nh->nexthop_id));
    g_hash_table_remove(g_static_nh_table, &nh->key); /* GDestroyNotify 释放 nh */
}

// ============================================================================
// RIB 写入 / 撤销（按组的模式统一推导参数）
// ============================================================================

static void nh_rib_params(const route_static_nh_t *nh, net_addr_t *source, net_addr_t *nexthop,
                          const net_addr_t **relay, uint32_t *relay_oif, uint32_t *out_ifindex, uint32_t *protocol)
{
    net_addr_t zero;
    zero_addr_with_family(&zero, nh->key.afi);

    if (nh->is_null0)
    {
        *protocol = ROUTE_PROTOCOL_STATIC;
        *source = zero;
        *nexthop = zero;
        *relay = NULL;
        *relay_oif = 0u;
        *out_ifindex = 0u;
    }
    else if (!nh->has_nexthop)
    {
        /* interface-only：source 用 ifindex 编码，nexthop 全零 */
        *protocol = ROUTE_PROTOCOL_STATIC;
        encode_ifindex_as_addr(nh->key.key_ifindex, source);
        *nexthop = zero;
        *relay = NULL;
        *relay_oif = nh->key.key_ifindex;
        *out_ifindex = nh->key.key_ifindex;
    }
    else
    {
        *protocol = ROUTE_PROTOCOL_STATIC;
        *source = nh->key.nexthop;
        *nexthop = nh->key.nexthop;
        *relay = &nh->relay_addr;
        *relay_oif = nh->relay_ifindex;
        *out_ifindex = nh->key.key_ifindex;
    }
}

/* 按组当前可达性写入或暂存某前缀：写入时只下发 nexthop_id（relay 已在对象里） */
static void route_apply(route_static_entry_t *entry, route_static_nh_t *nh)
{
    net_addr_t source;
    net_addr_t nexthop;
    const net_addr_t *relay = NULL;
    uint32_t relay_oif = 0u;
    uint32_t out_ifindex = 0u;
    uint32_t protocol = ROUTE_PROTOCOL_STATIC;
    nh_rib_params(nh, &source, &nexthop, &relay, &relay_oif, &out_ifindex, &protocol);

    if (nh->resolved)
    {
        uint8_t route_nh_type =
            (nh->resolved_nh_type == ROUTE_NH_TYPE_BLACKHOLE) ? ROUTE_NH_TYPE_BLACKHOLE : nh->key.nh_type;
        int ret = route_add_and_notify_nexthop_id(entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                                                  entry->key.prefix_len, protocol, &source, nh->nexthop_id,
                                                  entry->metric, entry->preference, out_ifindex, route_nh_type);
        if (ret >= 0)
        {
            entry->in_rib = 1u;
        }
    }
    else if (entry->in_rib)
    {
        route_rib_del(g_route_work_local->rib, entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                      entry->key.prefix_len, protocol, &source, on_static_rib_del, NULL);
        entry->in_rib = 0u;
    }
}

/* 撤销某前缀（删除场景） */
static void route_withdraw(route_static_entry_t *entry, route_static_nh_t *nh)
{
    if (!entry->in_rib)
    {
        return;
    }
    net_addr_t source;
    net_addr_t nexthop;
    const net_addr_t *relay = NULL;
    uint32_t relay_oif = 0u;
    uint32_t out_ifindex = 0u;
    uint32_t protocol = ROUTE_PROTOCOL_STATIC;
    nh_rib_params(nh, &source, &nexthop, &relay, &relay_oif, &out_ifindex, &protocol);

    route_rib_del(g_route_work_local->rib, entry->key.vrf_id, entry->key.afi, &entry->key.prefix_addr,
                  entry->key.prefix_len, protocol, &source, on_static_rib_del, NULL);
    entry->in_rib = 0u;
}

typedef struct
{
    uint32_t nexthop_id;
    route_static_nh_t *nh;
} apply_nh_ctx_t;

static void apply_routes_of_nh_cb(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    route_static_entry_t *entry = (route_static_entry_t *)value_ptr;
    apply_nh_ctx_t *ctx = (apply_nh_ctx_t *)user_data;
    if (entry->key.nexthop_id == ctx->nexthop_id)
    {
        route_apply(entry, ctx->nh);
    }
}

/* 整组重应用所有引用该 nexthop 的前缀 */
static void apply_routes_of_nh(route_static_nh_t *nh)
{
    apply_nh_ctx_t ctx = {.nexthop_id = nh->nexthop_id, .nh = nh};
    g_hash_table_foreach(g_static_route_table, apply_routes_of_nh_cb, &ctx);
}

// ============================================================================
// 添加候选静态路由
// ============================================================================

int route_static_add(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                     const net_addr_t *nexthop_addr, int32_t metric, int32_t preference, const char *out_ifname)
{
    if (!prefix_addr || !nexthop_addr || !g_static_nh_table || !g_route_work_local)
    {
        return -1;
    }

    const char *safe_ifname = (out_ifname && out_ifname[0]) ? out_ifname : "";
    int has_nh = !net_addr_is_zero(nexthop_addr);
    int is_null0 = (!has_nh && g_ascii_strcasecmp(safe_ifname, "null0") == 0);

    /* 配置/身份出接口：纯 nexthop=0；nexthop+interface / interface-only = 解析 ifname；null0=0 */
    uint32_t cfg_ifindex = 0u;
    if (!is_null0 && safe_ifname[0] != '\0')
    {
        cfg_ifindex = resolve_ifindex(safe_ifname);
    }

    /* 下一跳身份键（与 RIB 路由一致） */
    net_addr_t nh_for_key;
    if (has_nh)
    {
        nh_for_key = *nexthop_addr;
    }
    else
    {
        zero_addr_with_family(&nh_for_key, afi);
    }
    route_nhobj_key_t key;
    build_nh_key(&key, vrf_id, afi, &nh_for_key, cfg_ifindex, is_null0);

    route_static_nh_t *nh = nh_get_or_create(&key, has_nh, is_null0, safe_ifname);
    if (!nh)
    {
        return -1;
    }

    /* 路由条目（前缀 + nexthop_id），upsert */
    route_static_entry_key_t ekey;
    memset(&ekey, 0, sizeof(ekey));
    ekey.vrf_id = vrf_id;
    ekey.afi = afi;
    ekey.prefix_len = prefix_len;
    ekey.prefix_addr = *prefix_addr;
    ekey.nexthop_id = nh->nexthop_id;

    route_static_entry_t *entry = (route_static_entry_t *)g_hash_table_lookup(g_static_route_table, &ekey);
    if (!entry)
    {
        entry = (route_static_entry_t *)g_malloc0(sizeof(*entry));
        if (!entry)
        {
            nh_unref(nh); /* 撤回本次可能新建的组 */
            return -1;
        }
        entry->key = ekey;
        g_hash_table_insert(g_static_route_table, &entry->key, entry);
        nh->refcount++;
    }
    entry->metric = metric;
    entry->preference = preference;

    route_apply(entry, nh);
    return 0;
}

// ============================================================================
// 删除候选静态路由（精确匹配 prefix + nexthop[+ifname]）
// ============================================================================

int route_static_del(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                     const net_addr_t *nexthop_addr, const char *out_ifname)
{
    if (!prefix_addr || !nexthop_addr || !g_static_nh_table || !g_route_work_local)
    {
        return -1;
    }

    const char *safe_ifname = (out_ifname && out_ifname[0]) ? out_ifname : "";
    int has_nh = !net_addr_is_zero(nexthop_addr);
    int is_null0 = (!has_nh && g_ascii_strcasecmp(safe_ifname, "null0") == 0);
    uint32_t cfg_ifindex = 0u;
    if (!is_null0 && safe_ifname[0] != '\0')
    {
        cfg_ifindex = resolve_ifindex(safe_ifname);
    }

    net_addr_t nh_for_key;
    if (has_nh)
    {
        nh_for_key = *nexthop_addr;
    }
    else
    {
        zero_addr_with_family(&nh_for_key, afi);
    }
    route_nhobj_key_t key;
    build_nh_key(&key, vrf_id, afi, &nh_for_key, cfg_ifindex, is_null0);

    route_static_nh_t *nh = (route_static_nh_t *)g_hash_table_lookup(g_static_nh_table, &key);
    if (!nh)
    {
        return 0;
    }

    route_static_entry_key_t ekey;
    memset(&ekey, 0, sizeof(ekey));
    ekey.vrf_id = vrf_id;
    ekey.afi = afi;
    ekey.prefix_len = prefix_len;
    ekey.prefix_addr = *prefix_addr;
    ekey.nexthop_id = nh->nexthop_id;

    route_static_entry_t *entry = (route_static_entry_t *)g_hash_table_lookup(g_static_route_table, &ekey);
    if (!entry)
    {
        return 0;
    }

    route_withdraw(entry, nh);
    g_hash_table_remove(g_static_route_table, &ekey); /* 释放 entry */
    nh_unref(nh);
    return 1;
}

// ============================================================================
// 批量删除：按前缀 / 按 VRF
// ============================================================================

typedef struct
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t prefix_len;
    int by_prefix; /* 1=按前缀匹配；0=按 VRF 匹配 */
    const net_addr_t *prefix_addr;
    GSList *entries; /* 待删除的 entry 指针 */
} static_collect_ctx_t;

static void static_collect_cb(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    route_static_entry_t *entry = (route_static_entry_t *)value_ptr;
    static_collect_ctx_t *ctx = (static_collect_ctx_t *)user_data;

    if (entry->key.vrf_id != ctx->vrf_id)
    {
        return;
    }
    if (ctx->by_prefix)
    {
        if (entry->key.afi != ctx->afi || entry->key.prefix_len != ctx->prefix_len ||
            !net_addr_equal(&entry->key.prefix_addr, ctx->prefix_addr))
        {
            return;
        }
    }
    ctx->entries = g_slist_prepend(ctx->entries, entry);
}

static int static_del_collected(static_collect_ctx_t *ctx)
{
    int count = 0;
    for (GSList *l = ctx->entries; l; l = l->next)
    {
        route_static_entry_t *entry = (route_static_entry_t *)l->data;
        route_static_nh_t *nh =
            (route_static_nh_t *)g_hash_table_lookup(g_static_nh_by_id, GUINT_TO_POINTER(entry->key.nexthop_id));
        if (nh)
        {
            route_withdraw(entry, nh);
        }
        route_static_entry_key_t ekey = entry->key;
        g_hash_table_remove(g_static_route_table, &ekey); /* 释放 entry，之后不可再用 entry */
        if (nh)
        {
            nh_unref(nh);
        }
        count++;
    }
    g_slist_free(ctx->entries);
    ctx->entries = NULL;
    return count;
}

int route_static_del_prefix(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len)
{
    if (!prefix_addr || !g_static_route_table || !g_route_work_local)
    {
        return -1;
    }
    static_collect_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vrf_id = vrf_id;
    ctx.afi = afi;
    ctx.prefix_len = prefix_len;
    ctx.by_prefix = 1;
    ctx.prefix_addr = prefix_addr;
    g_hash_table_foreach(g_static_route_table, static_collect_cb, &ctx);
    return static_del_collected(&ctx);
}

int route_static_del_vrf(uint32_t vrf_id)
{
    if (!g_static_route_table || !g_route_work_local)
    {
        return 0;
    }
    static_collect_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vrf_id = vrf_id;
    ctx.by_prefix = 0;
    g_hash_table_foreach(g_static_route_table, static_collect_cb, &ctx);
    return static_del_collected(&ctx);
}

// ============================================================================
// nexthop 可达性变化（relay 统一回调）：按下一跳组驱动
// ============================================================================

typedef struct
{
    uint32_t nexthop_id;
    int resolved;
    uint8_t resolved_nh_type;
    const net_addr_t *gateway;
    uint32_t out_ifindex;
    uint32_t changed;
} static_nh_change_ctx_t;

static void static_nh_change_cb(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    route_static_nh_t *nh = (route_static_nh_t *)value_ptr;
    static_nh_change_ctx_t *ctx = (static_nh_change_ctx_t *)user_data;

    if (!nh->has_nexthop || nh->nexthop_id != ctx->nexthop_id)
    {
        return;
    }

    int resolved = ctx->resolved;
    uint8_t resolved_nh_type = ctx->resolved_nh_type ? ctx->resolved_nh_type : ROUTE_NH_TYPE_IP;
    uint32_t oif = ctx->out_ifindex;
    net_addr_t relay;
    memset(&relay, 0, sizeof(relay));
    if (ctx->gateway)
    {
        relay = *ctx->gateway;
    }
    /* nexthop+interface：校验 relay 出接口与配置接口一致 */
    if (resolved && nh->key.key_ifindex != 0 && oif != nh->key.key_ifindex)
    {
        resolved = 0;
    }

    uint8_t new_resolved = resolved ? 1u : 0u;
    uint8_t new_nh_type = resolved ? resolved_nh_type : ROUTE_NH_TYPE_IP;
    if (nh->resolved == new_resolved && nh->resolved_nh_type == new_nh_type && nh->relay_ifindex == oif &&
        static_relay_addr_equal(&nh->relay_addr, &relay))
    {
        return;
    }

    nh->resolved = new_resolved;
    nh->resolved_nh_type = new_nh_type;
    if (resolved)
    {
        nh->relay_addr = relay;
        nh->relay_ifindex = oif;
        nh_set_object_relay(nh); /* 同步 relay 到 nexthop 对象（再按 id 整组重写） */
    }
    apply_routes_of_nh(nh);
    ctx->changed++;
}

void route_static_on_nh_change(uint32_t nexthop_id, int resolved, uint8_t resolved_nh_type, const net_addr_t *gateway,
                               uint32_t out_ifindex)
{
    if (!g_static_nh_table || g_hash_table_size(g_static_nh_table) == 0 || nexthop_id == 0u)
    {
        return;
    }
    static_nh_change_ctx_t ctx = {
        .nexthop_id = nexthop_id,
        .resolved = resolved,
        .resolved_nh_type = resolved_nh_type,
        .gateway = gateway,
        .out_ifindex = out_ifindex,
        .changed = 0u,
    };
    g_hash_table_foreach(g_static_nh_table, static_nh_change_cb, &ctx);
}

// ============================================================================
// 接口状态变化：重检查 interface-only 组
// ============================================================================

static void static_if_change_cb(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    (void)user_data;
    route_static_nh_t *nh = (route_static_nh_t *)value_ptr;

    /* 仅 interface-only 组（null0 永远有效；IP nexthop 走 on_nh_change） */
    if (nh->has_nexthop || nh->is_null0)
    {
        return;
    }

    uint32_t ifindex = nh->key.key_ifindex;
    int reachable = (ifindex != 0) && rib_has_active_connected(nh->key.vrf_id, nh->key.afi, ifindex);
    nh->resolved = reachable ? 1u : 0u;
    nh->resolved_nh_type = ROUTE_NH_TYPE_IP;
    nh->relay_ifindex = ifindex;
    nh_set_object_relay(nh); /* 同步 relay 到 nexthop 对象 */
    apply_routes_of_nh(nh);
}

void route_static_on_if_change(void)
{
    if (!g_static_nh_table || g_hash_table_size(g_static_nh_table) == 0)
    {
        return;
    }
    g_hash_table_foreach(g_static_nh_table, static_if_change_cb, NULL);
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

    if (ctx->has_afi_filter && entry->key.afi != ctx->afi_filter)
    {
        return;
    }
    if (entry->key.vrf_id != ctx->vrf_filter)
    {
        return;
    }

    const route_static_nh_t *nh =
        (const route_static_nh_t *)g_hash_table_lookup(g_static_nh_by_id, GUINT_TO_POINTER(entry->key.nexthop_id));

    char prefix_str[64];
    char nh_str[64];
    char prefix_with_len[80];
    net_addr_to_str(&entry->key.prefix_addr, prefix_str, sizeof(prefix_str));
    if (nh && nh->has_nexthop)
    {
        net_addr_to_str(&nh->key.nexthop, nh_str, sizeof(nh_str));
    }
    else
    {
        g_strlcpy(nh_str, "-", sizeof(nh_str));
    }
    snprintf(prefix_with_len, sizeof(prefix_with_len), "%s/%u", prefix_str, (unsigned)entry->key.prefix_len);

    const char *afi_str = (entry->key.afi == ROUTE_AFI_IPV4) ? "ipv4" : "ipv6";
    const char *ifname = (nh && nh->out_ifname[0]) ? nh->out_ifname : "-";
    const char *resolved_str = (nh && nh->resolved) ? "yes" : "no ";
    const char *in_rib_str = entry->in_rib ? "yes" : "no ";

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

    if (!g_static_route_table || g_hash_table_size(g_static_route_table) == 0)
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
    g_hash_table_foreach(g_static_route_table, static_show_cb, &ctx);

    g_string_append_printf(buf, "\r\nTotal %u static route(s) in candidate table\r\n", ctx.count);
}

// ============================================================================
// show route static nexthop（按下一跳分组）
// ============================================================================

typedef struct
{
    GString *buf;
    int has_afi;
    uint16_t afi;
    int has_vrf;
    uint32_t vrf_id;
    int has_nhid;
    uint32_t nexthop_id;
    uint32_t count;
} static_nh_show_ctx_t;

static const char *static_nh_kind(const route_static_nh_t *nh)
{
    if (nh->is_null0)
    {
        return "null0";
    }
    if (!nh->has_nexthop)
    {
        return "iface";
    }
    return "ip";
}

static void static_nh_show_cb(gpointer key_ptr, gpointer value_ptr, gpointer user_data)
{
    (void)key_ptr;
    const route_static_nh_t *nh = (const route_static_nh_t *)value_ptr;
    static_nh_show_ctx_t *ctx = (static_nh_show_ctx_t *)user_data;
    if (ctx->has_afi && nh->key.afi != ctx->afi)
    {
        return;
    }
    if (ctx->has_vrf && nh->key.vrf_id != ctx->vrf_id)
    {
        return;
    }
    if (ctx->has_nhid && nh->nexthop_id != ctx->nexthop_id)
    {
        return;
    }

    char nh_str[64] = "-";
    char relay_str[64] = "-";
    if (nh->has_nexthop)
    {
        net_addr_to_str(&nh->key.nexthop, nh_str, sizeof(nh_str));
    }
    if (!net_addr_is_zero(&nh->relay_addr))
    {
        net_addr_to_str(&nh->relay_addr, relay_str, sizeof(relay_str));
    }
    const char *ifname = nh->out_ifname[0] ? nh->out_ifname : "-";

    g_string_append_printf(ctx->buf, "%-10u %-6u %-5s %-6s %-20s %-20s %-10s %-6u %-8s %-5u\r\n", nh->nexthop_id,
                           nh->key.vrf_id, (nh->key.afi == ROUTE_AFI_IPV6) ? "ipv6" : "ipv4", static_nh_kind(nh),
                           nh_str, relay_str, ifname, nh->relay_ifindex, nh->resolved ? "yes" : "no", nh->refcount);
    ctx->count++;
}

void route_static_show_nexthop(GString *buf, int has_afi, uint16_t afi, int has_vrf, uint32_t vrf_id, int has_nhid,
                               uint32_t nexthop_id)
{
    if (!buf)
    {
        return;
    }
    g_string_append_printf(
        buf,
        "\r\nStatic Nexthop Groups\r\n"
        "%-10s %-6s %-5s %-6s %-20s %-20s %-10s %-6s %-8s %-5s\r\n"
        "---------- ------ ----- ------ -------------------- -------------------- ---------- ------ -------- -----\r\n",
        "NH-ID", "VRF", "AFI", "Kind", "Nexthop", "Relay", "Interface", "OIF", "Resolved", "Ref");

    if (!g_static_nh_table || g_hash_table_size(g_static_nh_table) == 0)
    {
        g_string_append(buf, "  (no entries)\r\n");
        g_string_append(buf, "\r\nTotal 0 static nexthop(s)\r\n");
        return;
    }

    static_nh_show_ctx_t ctx = {
        .buf = buf,
        .has_afi = has_afi,
        .afi = afi,
        .has_vrf = has_vrf,
        .vrf_id = vrf_id,
        .has_nhid = has_nhid,
        .nexthop_id = nexthop_id,
        .count = 0,
    };
    g_hash_table_foreach(g_static_nh_table, static_nh_show_cb, &ctx);
    g_string_append_printf(buf, "\r\nTotal %u static nexthop(s)\r\n", ctx.count);
}

void route_static_show_relay(GString *buf, uint16_t afi_filter, int has_afi_filter, uint32_t vrf_filter,
                             const char *vrf_name)
{
    if (!buf)
    {
        return;
    }
    route_relay_show(buf, DEV_MODULE_ID_ROUTE, 1, afi_filter, has_afi_filter, vrf_filter, vrf_name);
}
