/**
 * @file   bgp_nexthop.c
 * @brief  BGP nexthop object registry.
 */
#include "bgp_nexthop.h"

#include <glib.h>
#include <string.h>

#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_protocol.h"
#include "bgp_vrf.h"
#include "errcode.h"
#include "log.h"

#define BGP_NHOBJ_RPC_TIMEOUT_MS 3000u

/*
 * nexthop id 仍由 ROUTE 分配（acquire 时 want_id=0 → ROUTE 返回新 id），BGP 把它存为
 * route->nexthop_id 当作稳定 id。BGP 原始下一跳地址保存在本表 entry->key.nexthop 中；
 * route 节点只保存 id，不再复制 bgp_nexthop_t。
 * ROUTE 进程重启后，BGP 用同一批 id 反刷（bgp_nexthop_resync_all 重发 acquire，want_id=旧 id），
 * ROUTE 见到 want_id!=0 时按该 id 恢复对象 → id 不变，route 与 relay watch 都无需改动。
 */
typedef struct bgp_nexthop_entry
{
    route_nhobj_key_t key;
    bgp_nexthop_value_t value;
    uint32_t id; /**< ROUTE 分配的稳定 id（route->nexthop_id；重启后用同一 id 反刷恢复） */
    uint32_t refcount;
} bgp_nexthop_entry_t;

static gboolean bgp_route_is_local_cross_uc(const bgp_route_node_t *route)
{
    return route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) && route->head && route->head->inst &&
           route->head->inst->vrf && route->head->inst->vrf->vrf_id != BGP_VRF_PUBLIC_ID &&
           route->head->inst->safi == BGP_SAFI_UNICAST;
}

static const bgp_route_node_t *bgp_nexthop_route_owner(const bgp_route_node_t *route)
{
    if (bgp_route_is_local_cross_uc(route))
    {
        return route;
    }
    return (route && route->src_route) ? route->src_route : route;
}

/**
 * @brief 向 ROUTE 发送 acquire 并取回 id
 * @param key     nexthop 身份键
 * @param want_id 期望 id（0=首次申请，由 ROUTE 分配；非 0=重启反刷，要求 ROUTE 按该 id 恢复）
 * @param id_out  ROUTE 返回的 id
 */
static int bgp_nh_route_acquire(const route_nhobj_key_t *key, uint32_t want_id, uint32_t *id_out)
{
    route_nhobj_msg_t req;
    memset(&req, 0, sizeof(req));
    req.key = *key;
    req.nexthop_id = want_id; /* 0=ROUTE 分配；非 0=要求 ROUTE 恢复该 id */
    req.relay_addr = key->nexthop;
    req.relay_ifindex = 0u;

    uint32_t got = 0u;
    if (route_rpc_nhobj_acquire_wait(bgp_local_ipc_ctx(), &req, BGP_NHOBJ_RPC_TIMEOUT_MS, &got) != ERRCODE_SUCCESS ||
        got == 0u || (want_id != 0u && got != want_id))
    {
        return ERRCODE_FAIL;
    }
    if (id_out)
    {
        *id_out = got;
    }
    return ERRCODE_SUCCESS;
}

static guint bgp_nexthop_key_hash(gconstpointer p)
{
    const route_nhobj_key_t *k = (const route_nhobj_key_t *)p;
    if (!k)
    {
        return 0u;
    }

    guint h = (guint)k->vrf_id;
    h = h * 33u + (guint)k->protocol;
    h = h * 33u + (guint)k->afi;
    h = h * 33u + (guint)k->nh_type;
    h = h * 33u + (guint)k->key_ifindex;
    h ^= net_addr_hash(&k->nexthop);
    return h;
}

int bgp_nexthop_key_equal(const route_nhobj_key_t *a, const route_nhobj_key_t *b)
{
    if (!a || !b)
    {
        return 0;
    }

    return a->vrf_id == b->vrf_id && a->protocol == b->protocol && a->afi == b->afi && a->nh_type == b->nh_type &&
           a->key_ifindex == b->key_ifindex && net_addr_equal(&a->nexthop, &b->nexthop);
}

static gboolean bgp_nexthop_key_hash_equal(gconstpointer a, gconstpointer b)
{
    return bgp_nexthop_key_equal((const route_nhobj_key_t *)a, (const route_nhobj_key_t *)b) ? TRUE : FALSE;
}

void bgp_nexthop_make_route_key(const bgp_route_node_t *route, const net_addr_t *nexthop, route_nhobj_key_t *key)
{
    if (!key)
    {
        return;
    }

    memset(key, 0, sizeof(*key));
    key->vrf_id = (route && route->head && route->head->inst && route->head->inst->vrf) ? route->head->inst->vrf->vrf_id
                                                                                        : ROUTE_VRF_DEFAULT;
    /* vrf 本地交叉(LOCAL_CROSS)：本路由落在目标 VRF，但下一跳要在「源 VRF」里迭代解析(nexthop-vrf)。
     * 让 nexthop 对象的 key.vrf_id=源 VRF，ROUTE 即在源 VRF 迭代该对象，BGP 收解析结果后下刷目标 VRF FIB。 */
    if (route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) && route->src_route && route->src_route->head &&
        route->src_route->head->inst && route->src_route->head->inst->vrf)
    {
        key->vrf_id = route->src_route->head->inst->vrf->vrf_id;
        route_nhobj_key_t src_key;
        if (bgp_nexthop_get_route_key(route->src_route, &src_key) == ERRCODE_SUCCESS)
        {
            key->nh_type = src_key.nh_type;
        }
    }
    key->protocol = ROUTE_PROTOCOL_BGP;
    key->afi = (route && route->head) ? (uint16_t)route->head->nlri.afi : ROUTE_AFI_IPV4;
    if (key->nh_type == 0u)
    {
        key->nh_type = ROUTE_NH_TYPE_IP;
    }
    key->key_ifindex = ROUTE_NHOBJ_KEY_IFINDEX_LOCAL_CROSS;
    if (route && (!BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) || !route->src_route || !route->src_route->head ||
                  !route->src_route->head->inst || !route->src_route->head->inst->vrf))
    {
        key->key_ifindex = 0u;
    }
    if (nexthop)
    {
        key->nexthop = *nexthop;
    }
}

void bgp_nexthop_init(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }
    if (!inst->nexthop_by_key)
    {
        inst->nexthop_by_key = g_hash_table_new_full(bgp_nexthop_key_hash, bgp_nexthop_key_hash_equal, NULL, g_free);
    }
    if (!inst->nexthop_by_id)
    {
        inst->nexthop_by_id = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
    }
}

void bgp_nexthop_cleanup(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }
    if (inst->nexthop_by_key)
    {
        GHashTableIter iter;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, inst->nexthop_by_key);
        while (g_hash_table_iter_next(&iter, NULL, &value))
        {
            const bgp_nexthop_entry_t *entry = (const bgp_nexthop_entry_t *)value;
            if (entry && entry->id != 0u)
            {
                (void)route_rpc_nhobj_release(bgp_local_ipc_ctx(), entry->id);
            }
        }
    }

    if (inst->nexthop_by_id)
    {
        g_hash_table_destroy(inst->nexthop_by_id);
        inst->nexthop_by_id = NULL;
    }
    if (inst->nexthop_by_key)
    {
        g_hash_table_destroy(inst->nexthop_by_key);
        inst->nexthop_by_key = NULL;
    }
}

int bgp_nexthop_acquire(bgp_instance_t *inst, const route_nhobj_key_t *key, uint32_t *id_out)
{
    if (!inst || !key || !id_out || !inst->nexthop_by_key || !inst->nexthop_by_id)
    {
        return ERRCODE_FAIL;
    }

    bgp_nexthop_entry_t *entry = (bgp_nexthop_entry_t *)g_hash_table_lookup(inst->nexthop_by_key, key);
    if (entry)
    {
        entry->refcount++;
        *id_out = entry->id;
        return ERRCODE_SUCCESS;
    }

    /* 首次申请：want_id=0，由 ROUTE 分配 id 并返回 */
    uint32_t id = 0u;
    if (bgp_nh_route_acquire(key, 0u, &id) != ERRCODE_SUCCESS || id == 0u)
    {
        return ERRCODE_FAIL;
    }

    entry = g_malloc0(sizeof(*entry));
    if (!entry)
    {
        (void)route_rpc_nhobj_release(bgp_local_ipc_ctx(), id);
        return ERRCODE_FAIL;
    }

    entry->key = *key;
    memset(&entry->value, 0, sizeof(entry->value));
    entry->value.updated_at_usec = g_get_real_time();
    entry->id = id;
    entry->refcount = 1u;
    g_hash_table_insert(inst->nexthop_by_key, &entry->key, entry);
    g_hash_table_insert(inst->nexthop_by_id, GUINT_TO_POINTER(id), entry);

    *id_out = id;
    return ERRCODE_SUCCESS;
}

int bgp_nexthop_retain(bgp_instance_t *inst, uint32_t id)
{
    if (!inst || id == 0u || !inst->nexthop_by_id)
    {
        return ERRCODE_FAIL;
    }

    bgp_nexthop_entry_t *entry = (bgp_nexthop_entry_t *)g_hash_table_lookup(inst->nexthop_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    entry->refcount++;
    return ERRCODE_SUCCESS;
}

void bgp_nexthop_release(bgp_instance_t *inst, uint32_t id)
{
    if (!inst || id == 0u || !inst->nexthop_by_id || !inst->nexthop_by_key)
    {
        return;
    }

    bgp_nexthop_entry_t *entry = (bgp_nexthop_entry_t *)g_hash_table_lookup(inst->nexthop_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return;
    }

    if (entry->refcount > 1u)
    {
        entry->refcount--;
        return;
    }

    (void)route_rpc_nhobj_release(bgp_local_ipc_ctx(), id);
    g_hash_table_remove(inst->nexthop_by_id, GUINT_TO_POINTER(id));
    g_hash_table_remove(inst->nexthop_by_key, &entry->key);
}

int bgp_nexthop_lookup(bgp_instance_t *inst, uint32_t id, route_nhobj_key_t *key_out)
{
    if (!inst || id == 0u || !key_out || !inst->nexthop_by_id)
    {
        return ERRCODE_FAIL;
    }

    bgp_nexthop_entry_t *entry = (bgp_nexthop_entry_t *)g_hash_table_lookup(inst->nexthop_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    *key_out = entry->key;
    return ERRCODE_SUCCESS;
}

int bgp_nexthop_get_value(bgp_instance_t *inst, uint32_t id, bgp_nexthop_value_t *value_out)
{
    if (!inst || id == 0u || !value_out || !inst->nexthop_by_id)
    {
        return ERRCODE_FAIL;
    }

    bgp_nexthop_entry_t *entry = (bgp_nexthop_entry_t *)g_hash_table_lookup(inst->nexthop_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    *value_out = entry->value;
    return ERRCODE_SUCCESS;
}

int bgp_nexthop_get_route_key(const bgp_route_node_t *route, route_nhobj_key_t *key_out)
{
    const bgp_route_node_t *owner = bgp_nexthop_route_owner(route);
    if (!owner || owner->nexthop_id == 0u || !owner->head || !owner->head->inst || !key_out)
    {
        return ERRCODE_FAIL;
    }

    return bgp_nexthop_lookup(owner->head->inst, owner->nexthop_id, key_out);
}

int bgp_nexthop_get_route_addr(const bgp_route_node_t *route, net_addr_t *nexthop_out)
{
    if (!nexthop_out)
    {
        return ERRCODE_FAIL;
    }

    route_nhobj_key_t key;
    if (bgp_nexthop_get_route_key(route, &key) != ERRCODE_SUCCESS || key.nexthop.family == 0)
    {
        memset(nexthop_out, 0, sizeof(*nexthop_out));
        return ERRCODE_FAIL;
    }

    *nexthop_out = key.nexthop;
    return ERRCODE_SUCCESS;
}

int bgp_nexthop_get_route_bgp(const bgp_route_node_t *route, bgp_nexthop_t *nexthop_out)
{
    if (!nexthop_out)
    {
        return ERRCODE_FAIL;
    }

    memset(nexthop_out, 0, sizeof(*nexthop_out));
    if (bgp_nexthop_get_route_addr(route, &nexthop_out->global) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    nexthop_out->has_link_local = false;
    return ERRCODE_SUCCESS;
}

int bgp_nexthop_set_value(bgp_instance_t *inst, uint32_t id, const bgp_nexthop_value_t *value)
{
    if (!inst || id == 0u || !value || !inst->nexthop_by_id)
    {
        return ERRCODE_FAIL;
    }

    bgp_nexthop_entry_t *entry = (bgp_nexthop_entry_t *)g_hash_table_lookup(inst->nexthop_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    entry->value = *value;
    entry->value.updated_at_usec = g_get_real_time();
    return ERRCODE_SUCCESS;
}

void bgp_nexthop_clear_value(bgp_instance_t *inst, uint32_t id)
{
    if (!inst || id == 0u || !inst->nexthop_by_id)
    {
        return;
    }

    bgp_nexthop_entry_t *entry = (bgp_nexthop_entry_t *)g_hash_table_lookup(inst->nexthop_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return;
    }

    memset(&entry->value, 0, sizeof(entry->value));
    entry->value.updated_at_usec = g_get_real_time();
}

int bgp_nexthop_set_route_key(bgp_route_node_t *route, const route_nhobj_key_t *key)
{
    if (!route || !route->head || !route->head->inst || !key || key->nexthop.family == 0 || key->nh_type == 0u)
    {
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = route->head->inst;

    route_nhobj_key_t cur_key;
    if (route->nexthop_id != 0u && bgp_nexthop_lookup(inst, route->nexthop_id, &cur_key) == ERRCODE_SUCCESS &&
        bgp_nexthop_key_equal(&cur_key, key))
    {
        return ERRCODE_SUCCESS;
    }

    uint32_t new_id = 0u;
    if (bgp_nexthop_acquire(inst, key, &new_id) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    bgp_nexthop_reset_route(route);
    route->nexthop_id = new_id;
    return ERRCODE_SUCCESS;
}

int bgp_nexthop_set_route(bgp_route_node_t *route, const bgp_nexthop_t *nexthop)
{
    if (!route || !route->head || !route->head->inst || !nexthop || nexthop->global.family == 0)
    {
        return ERRCODE_FAIL;
    }

    route_nhobj_key_t key;
    bgp_nexthop_make_route_key(route, &nexthop->global, &key);
    return bgp_nexthop_set_route_key(route, &key);
}

void bgp_nexthop_reset_route(bgp_route_node_t *route)
{
    if (!route || route->nexthop_id == 0u)
    {
        return;
    }
    bgp_instance_t *inst = (route->head) ? route->head->inst : NULL;
    bgp_nexthop_release(inst, route->nexthop_id);
    route->nexthop_id = 0u;
}

/** 对单个 instance 的所有 nexthop 对象按原 id 反刷到 ROUTE（ROUTE 重启后重建） */
static uint32_t bgp_nexthop_resync_inst(bgp_instance_t *inst)
{
    if (!inst || !inst->nexthop_by_key)
    {
        return 0u;
    }

    uint32_t pushed = 0u;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->nexthop_by_key);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        bgp_nexthop_entry_t *entry = (bgp_nexthop_entry_t *)value;
        if (!entry || entry->id == 0u)
        {
            continue;
        }
        if (bgp_nh_route_acquire(&entry->key, entry->id, NULL) == ERRCODE_SUCCESS)
        {
            pushed++;
        }
        else
        {
            LOG_WARN("BGP: nexthop resync push failed id=%u", entry->id);
        }
    }
    return pushed;
}

uint32_t bgp_nexthop_resync_all(bgp_protocol_t *proto)
{
    if (!proto || !proto->vrf_hash)
    {
        return 0u;
    }

    uint32_t pushed = 0u;
    GHashTableIter vit;
    gpointer vv = NULL;
    g_hash_table_iter_init(&vit, proto->vrf_hash);
    while (g_hash_table_iter_next(&vit, NULL, &vv))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)vv;
        if (!vrf || !vrf->inst_hash)
        {
            continue;
        }
        GHashTableIter iit;
        gpointer iv = NULL;
        g_hash_table_iter_init(&iit, vrf->inst_hash);
        while (g_hash_table_iter_next(&iit, NULL, &iv))
        {
            pushed += bgp_nexthop_resync_inst((bgp_instance_t *)iv);
        }
    }

    if (pushed > 0u)
    {
        LOG_INFO("BGP: re-pushed %u nexthop object(s) to ROUTE after restart", pushed);
    }
    return pushed;
}
