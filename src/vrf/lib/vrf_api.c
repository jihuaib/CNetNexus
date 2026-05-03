/**
 * @file   vrf_api.c
 * @brief  VRF 模块对外 API 实现：RPC、订阅、客户端缓存
 * @author jhb
 * @date   2026/03/05
 */
#include <glib.h>
#include <string.h>

#include "errcode.h"
#include "log.h"
#include "vrf.h"

// ============================================================================
// RPC：vrf_get_name
// ============================================================================

int vrf_get_name(dev_ipc_context_t *ctx, uint32_t vrf_id, char *name_out, size_t name_size)
{
    if (!ctx || !name_out || name_size == 0)
    {
        return ERRCODE_FAIL;
    }

    name_out[0] = '\0';

    dev_ipc_message_t *req = dev_ipc_message_create(VRF_MSG_TYPE_GET_NAME, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_VRF, 0, &vrf_id, sizeof(uint32_t), NULL);
    if (!req)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_VRF, req, 0);
    dev_ipc_message_free(req);

    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    if (resp->payload && resp->payload_len > 0)
    {
        strlcpy(name_out, (const char *)resp->payload, name_size);
        dev_ipc_message_free(resp);
        return ERRCODE_SUCCESS;
    }

    dev_ipc_message_free(resp);
    return ERRCODE_FAIL;
}

// ============================================================================
// 客户端缓存：内部数据结构
// ============================================================================

/* key = GUINT_TO_POINTER(vrf_id), value = vrf_api_cache_entry_t* */
static GHashTable *g_vrf_cache_by_id = NULL;
/* key = entry->name (借用 entry 内部缓冲区), value = vrf_api_cache_entry_t* */
static GHashTable *g_vrf_cache_by_name = NULL;

static gpointer af_key_make(uint16_t afi, uint8_t safi)
{
    return GUINT_TO_POINTER(((guint32)afi << 8) | (guint32)safi);
}

static void af_destroy(gpointer data)
{
    vrf_api_af_t *af = (vrf_api_af_t *)data;
    if (!af)
    {
        return;
    }
    g_free(af->import_rts);
    g_free(af->export_rts);
    g_free(af);
}

static vrf_api_cache_entry_t *cache_entry_new(uint32_t vrf_id, const char *name, uint32_t l3vrf_table_id)
{
    vrf_api_cache_entry_t *e = g_malloc0(sizeof(*e));
    e->vrf_id = vrf_id;
    if (name)
    {
        g_strlcpy(e->name, name, sizeof(e->name));
    }
    e->l3vrf_table_id = l3vrf_table_id;
    e->os_state = VRF_OS_STATE_UNKNOWN;
    e->afs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, af_destroy);
    return e;
}

static void cache_entry_destroy(gpointer data)
{
    vrf_api_cache_entry_t *e = (vrf_api_cache_entry_t *)data;
    if (!e)
    {
        return;
    }
    if (e->afs)
    {
        g_hash_table_destroy(e->afs);
        e->afs = NULL;
    }
    g_free(e);
}

static vrf_api_cache_entry_t *cache_get_or_create(uint32_t vrf_id, const char *name, uint32_t l3vrf_table_id)
{
    if (!g_vrf_cache_by_id)
    {
        return NULL;
    }
    vrf_api_cache_entry_t *e = g_hash_table_lookup(g_vrf_cache_by_id, GUINT_TO_POINTER(vrf_id));
    if (e)
    {
        if (name && name[0] != '\0' && strcmp(e->name, name) != 0)
        {
            g_hash_table_remove(g_vrf_cache_by_name, e->name);
            g_strlcpy(e->name, name, sizeof(e->name));
            g_hash_table_insert(g_vrf_cache_by_name, e->name, e);
        }
        if (l3vrf_table_id != 0)
        {
            e->l3vrf_table_id = l3vrf_table_id;
        }
        return e;
    }

    e = cache_entry_new(vrf_id, name, l3vrf_table_id);
    g_hash_table_insert(g_vrf_cache_by_id, GUINT_TO_POINTER(vrf_id), e);
    if (e->name[0] != '\0')
    {
        g_hash_table_insert(g_vrf_cache_by_name, e->name, e);
    }
    return e;
}

static vrf_api_af_t *af_get_or_create(vrf_api_cache_entry_t *e, uint16_t afi, uint8_t safi)
{
    if (!e || !e->afs)
    {
        return NULL;
    }
    gpointer key = af_key_make(afi, safi);
    vrf_api_af_t *af = g_hash_table_lookup(e->afs, key);
    if (af)
    {
        return af;
    }
    af = g_malloc0(sizeof(*af));
    af->afi = afi;
    af->safi = safi;
    g_hash_table_insert(e->afs, key, af);
    return af;
}

static void af_replace_rts(vrf_rt_t **slot, uint16_t *count_slot, const vrf_rt_t *rts, uint16_t count)
{
    g_free(*slot);
    *slot = NULL;
    *count_slot = 0;

    if (count == 0 || !rts)
    {
        return;
    }
    *slot = g_malloc(sizeof(vrf_rt_t) * count);
    memcpy(*slot, rts, sizeof(vrf_rt_t) * count);
    *count_slot = count;
}

// ============================================================================
// 客户端缓存：生命周期与查询
// ============================================================================

void vrf_api_cache_init(void)
{
    if (g_vrf_cache_by_id)
    {
        return;
    }
    g_vrf_cache_by_id = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, cache_entry_destroy);
    g_vrf_cache_by_name = g_hash_table_new(g_str_hash, g_str_equal);
    LOG_INFO("VRF API cache initialized");
}

void vrf_api_cache_cleanup(void)
{
    if (g_vrf_cache_by_name)
    {
        g_hash_table_destroy(g_vrf_cache_by_name);
        g_vrf_cache_by_name = NULL;
    }
    if (g_vrf_cache_by_id)
    {
        g_hash_table_destroy(g_vrf_cache_by_id);
        g_vrf_cache_by_id = NULL;
    }
}

const vrf_api_cache_entry_t *vrf_api_cache_lookup(uint32_t vrf_id)
{
    if (!g_vrf_cache_by_id)
    {
        return NULL;
    }
    return g_hash_table_lookup(g_vrf_cache_by_id, GUINT_TO_POINTER(vrf_id));
}

const vrf_api_cache_entry_t *vrf_api_cache_lookup_by_name(const char *name)
{
    if (!g_vrf_cache_by_name || !name)
    {
        return NULL;
    }
    return g_hash_table_lookup(g_vrf_cache_by_name, name);
}

const vrf_api_af_t *vrf_api_cache_get_af(uint32_t vrf_id, uint16_t afi, uint8_t safi)
{
    const vrf_api_cache_entry_t *e = vrf_api_cache_lookup(vrf_id);
    if (!e || !e->afs)
    {
        return NULL;
    }
    return g_hash_table_lookup(e->afs, af_key_make(afi, safi));
}

void vrf_api_cache_foreach(vrf_api_cache_iter_fn iter_fn, void *user_data)
{
    if (!g_vrf_cache_by_id || !iter_fn)
    {
        return;
    }
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&iter, g_vrf_cache_by_id);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        (void)key;
        if (iter_fn((const vrf_api_cache_entry_t *)val, user_data))
        {
            break;
        }
    }
}

// ============================================================================
// 客户端缓存：事件应用
// ============================================================================

static size_t event_min_size(uint16_t rt_count)
{
    /* vrf_event_msg_t 已含 rts[1]，再追加 rt_count-1 个 RT */
    if (rt_count == 0)
    {
        return offsetof(vrf_event_msg_t, rts);
    }
    return offsetof(vrf_event_msg_t, rts) + sizeof(vrf_rt_t) * rt_count;
}

void vrf_api_cache_on_event(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < offsetof(vrf_event_msg_t, rts))
    {
        return;
    }
    if (!g_vrf_cache_by_id)
    {
        return;
    }

    const vrf_event_msg_t *evt = (const vrf_event_msg_t *)msg->payload;
    if (msg->payload_len < event_min_size(evt->rt_count))
    {
        LOG_WARN("VRF API: event payload truncated (len=%u rt_count=%u)", msg->payload_len, evt->rt_count);
        return;
    }

    switch (evt->event)
    {
        case VRF_EVENT_VRF_ADD:
        {
            vrf_api_cache_entry_t *e = cache_get_or_create(evt->vrf_id, evt->name, evt->l3vrf_table_id);
            if (e)
            {
                e->os_state = evt->os_state;
            }
            break;
        }
        case VRF_EVENT_VRF_DEL:
        {
            vrf_api_cache_entry_t *e = g_hash_table_lookup(g_vrf_cache_by_id, GUINT_TO_POINTER(evt->vrf_id));
            if (e)
            {
                g_hash_table_remove(g_vrf_cache_by_name, e->name);
                g_hash_table_remove(g_vrf_cache_by_id, GUINT_TO_POINTER(evt->vrf_id));
            }
            break;
        }
        case VRF_EVENT_VRF_STATE:
        {
            vrf_api_cache_entry_t *e = cache_get_or_create(evt->vrf_id, evt->name, evt->l3vrf_table_id);
            if (e)
            {
                e->os_state = evt->os_state;
            }
            break;
        }
        case VRF_EVENT_AF_ENABLE:
        {
            vrf_api_cache_entry_t *e = cache_get_or_create(evt->vrf_id, evt->name, evt->l3vrf_table_id);
            (void)af_get_or_create(e, evt->afi, evt->safi);
            break;
        }
        case VRF_EVENT_AF_DISABLE:
        {
            vrf_api_cache_entry_t *e = g_hash_table_lookup(g_vrf_cache_by_id, GUINT_TO_POINTER(evt->vrf_id));
            if (e && e->afs)
            {
                g_hash_table_remove(e->afs, af_key_make(evt->afi, evt->safi));
            }
            break;
        }
        case VRF_EVENT_AF_RD_CHANGE:
        {
            vrf_api_cache_entry_t *e = cache_get_or_create(evt->vrf_id, evt->name, evt->l3vrf_table_id);
            vrf_api_af_t *af = af_get_or_create(e, evt->afi, evt->safi);
            if (af)
            {
                af->has_rd = evt->has_rd;
                af->rd = evt->rd;
            }
            break;
        }
        case VRF_EVENT_AF_IMPORT_RT_CHG:
        {
            vrf_api_cache_entry_t *e = cache_get_or_create(evt->vrf_id, evt->name, evt->l3vrf_table_id);
            vrf_api_af_t *af = af_get_or_create(e, evt->afi, evt->safi);
            if (af)
            {
                af_replace_rts(&af->import_rts, &af->import_rt_count, evt->rts, evt->rt_count);
            }
            break;
        }
        case VRF_EVENT_AF_EXPORT_RT_CHG:
        {
            vrf_api_cache_entry_t *e = cache_get_or_create(evt->vrf_id, evt->name, evt->l3vrf_table_id);
            vrf_api_af_t *af = af_get_or_create(e, evt->afi, evt->safi);
            if (af)
            {
                af_replace_rts(&af->export_rts, &af->export_rt_count, evt->rts, evt->rt_count);
            }
            break;
        }
        default:
            LOG_DEBUG("VRF API: ignore unknown event 0x%08X", evt->event);
            break;
    }
}

// ============================================================================
// 订阅 / 取消订阅
// ============================================================================

static int subscribe_send(dev_ipc_context_t *ctx, uint32_t msg_type, uint32_t af_mask, uint32_t event_mask,
                          uint32_t flags)
{
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    vrf_subscribe_req_t *req = g_malloc0(sizeof(*req));
    req->af_mask = af_mask;
    req->event_mask = event_mask;
    req->flags = flags;

    dev_ipc_message_t *msg =
        dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_VRF, 0, req, sizeof(*req), g_free);
    if (!msg)
    {
        g_free(req);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_send(ctx, DEV_MODULE_ID_VRF, msg);
    dev_ipc_message_free(msg);
    return (ret == ERRCODE_SUCCESS) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int vrf_api_subscribe(dev_ipc_context_t *ctx, uint32_t af_mask, uint32_t event_mask, uint32_t flags)
{
    if (af_mask == 0 || event_mask == 0)
    {
        return ERRCODE_FAIL;
    }
    return subscribe_send(ctx, VRF_MSG_TYPE_SUBSCRIBE, af_mask, event_mask, flags);
}

int vrf_api_subscribe_all(dev_ipc_context_t *ctx)
{
    return vrf_api_subscribe(ctx, VRF_AF_MASK_ALL, VRF_EVENT_ALL, VRF_SUBSCRIBE_FLAG_REPLAY);
}

int vrf_api_unsubscribe(dev_ipc_context_t *ctx, uint32_t af_mask, uint32_t event_mask)
{
    return subscribe_send(ctx, VRF_MSG_TYPE_UNSUBSCRIBE, af_mask, event_mask, 0);
}
