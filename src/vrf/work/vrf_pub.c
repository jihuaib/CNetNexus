/**
 * @file   vrf_pub.c
 * @brief  VRF 事件发布与订阅实现（worker 线程内）
 * @author jhb
 * @date   2026/05/02
 */
#include "vrf_pub.h"

#include <stddef.h>
#include <string.h>

#include "errcode.h"
#include "log.h"
#include "vrf_worker.h"

static uint32_t af_to_mask(uint16_t afi)
{
    if (afi == VRF_AFI_IPV4)
    {
        return VRF_AF_MASK_IPV4;
    }
    if (afi == VRF_AFI_IPV6)
    {
        return VRF_AF_MASK_IPV6;
    }
    return 0;
}

static int subscriber_match(uint32_t event_mask, uint32_t af_mask, uint32_t event, uint16_t afi)
{
    if ((event_mask & event) == 0)
    {
        return 0;
    }
    if (afi != 0)
    {
        uint32_t bit = af_to_mask(afi);
        if (bit == 0 || (af_mask & bit) == 0)
        {
            return 0;
        }
    }
    return 1;
}

static size_t event_payload_size(uint16_t rt_count)
{
    if (rt_count == 0)
    {
        return offsetof(vrf_event_msg_t, rts);
    }
    return offsetof(vrf_event_msg_t, rts) + sizeof(vrf_rt_t) * rt_count;
}

static vrf_event_msg_t *build_event(const vrf_entry_t *e, uint32_t event, uint16_t afi, const vrf_af_state_t *af,
                                    const vrf_rt_t *rts, uint16_t rt_count, uint8_t rt_type, size_t *out_size)
{
    size_t sz = event_payload_size(rt_count);
    vrf_event_msg_t *evt = g_malloc0(sz);
    evt->event = event;
    evt->vrf_id = e ? e->vrf_id : 0;
    evt->afi = afi;
    evt->os_state = e ? e->os_state : VRF_OS_STATE_UNKNOWN;
    evt->l3vrf_table_id = e ? e->l3vrf_table_id : 0;
    if (e)
    {
        g_strlcpy(evt->name, e->name, sizeof(evt->name));
    }
    if (af)
    {
        evt->has_rd = af->has_rd;
        evt->rd = af->rd;
        evt->apply_label_mode = af->apply_label_mode;
    }
    evt->rt_type = rt_type;
    evt->rt_count = rt_count;
    if (rt_count > 0 && rts)
    {
        memcpy(evt->rts, rts, sizeof(vrf_rt_t) * rt_count);
    }
    *out_size = sz;
    return evt;
}

static void send_event_to(uint32_t module_id, const vrf_event_msg_t *src, size_t sz)
{
    dev_ipc_context_t *ctx = vrf_worker_ipc_ctx();
    if (!ctx)
    {
        return;
    }
    void *dup = g_memdup2(src, (guint)sz);
    if (!dup)
    {
        return;
    }
    dev_ipc_message_t *msg =
        dev_ipc_message_create(VRF_MSG_TYPE_EVENT, DEV_MODULE_ID_VRF, module_id, 0, dup, (uint32_t)sz, g_free);
    if (!msg)
    {
        g_free(dup);
        return;
    }
    if (dev_ipc_send(ctx, module_id, msg) != ERRCODE_SUCCESS)
    {
        LOG_WARN("VRF: failed to send event 0x%08X to module 0x%08X", src->event, module_id);
    }
    dev_ipc_message_free(msg);
}

static void notify_all(uint32_t event, const vrf_entry_t *e, uint16_t afi, const vrf_af_state_t *af,
                       const vrf_rt_t *rts, uint16_t rt_count, uint8_t rt_type)
{
    GList *subs = vrf_worker_subscribers();
    if (!subs)
    {
        return;
    }
    size_t sz = 0;
    vrf_event_msg_t *src = build_event(e, event, afi, af, rts, rt_count, rt_type, &sz);
    if (!src)
    {
        return;
    }
    for (GList *l = subs; l; l = l->next)
    {
        vrf_subscriber_t *sub = (vrf_subscriber_t *)l->data;
        if (!sub || !subscriber_match(sub->event_mask, sub->af_mask, event, afi))
        {
            continue;
        }
        send_event_to(sub->module_id, src, sz);
    }
    g_free(src);
}

void vrf_pub_notify_vrf_add(const vrf_entry_t *e)
{
    if (e)
    {
        notify_all(VRF_EVENT_VRF_ADD, e, 0, NULL, NULL, 0, VRF_RT_TYPE_VPN);
    }
}

void vrf_pub_notify_vrf_del(const vrf_entry_t *e)
{
    if (e)
    {
        notify_all(VRF_EVENT_VRF_DEL, e, 0, NULL, NULL, 0, VRF_RT_TYPE_VPN);
    }
}

void vrf_pub_notify_vrf_state(const vrf_entry_t *e)
{
    if (e)
    {
        notify_all(VRF_EVENT_VRF_STATE, e, 0, NULL, NULL, 0, VRF_RT_TYPE_VPN);
    }
}

void vrf_pub_notify_af_enable(const vrf_entry_t *e, uint16_t afi)
{
    if (e)
    {
        notify_all(VRF_EVENT_AF_ENABLE, e, afi, NULL, NULL, 0, VRF_RT_TYPE_VPN);
    }
}

void vrf_pub_notify_af_disable(const vrf_entry_t *e, uint16_t afi)
{
    if (e)
    {
        notify_all(VRF_EVENT_AF_DISABLE, e, afi, NULL, NULL, 0, VRF_RT_TYPE_VPN);
    }
}

void vrf_pub_notify_af_rd_add(const vrf_entry_t *e, const vrf_af_state_t *af)
{
    if (e && af && af->has_rd)
    {
        notify_all(VRF_EVENT_AF_RD_ADD, e, af->afi, af, NULL, 0, VRF_RT_TYPE_VPN);
    }
}

void vrf_pub_notify_af_rd_del(const vrf_entry_t *e, uint16_t afi)
{
    if (e)
    {
        notify_all(VRF_EVENT_AF_RD_DEL, e, afi, NULL, NULL, 0, VRF_RT_TYPE_VPN);
    }
}

void vrf_pub_notify_af_apply_label(const vrf_entry_t *e, const vrf_af_state_t *af)
{
    if (e && af)
    {
        notify_all(VRF_EVENT_AF_APPLY_LABEL, e, af->afi, af, NULL, 0, VRF_RT_TYPE_VPN);
    }
}

static void notify_rt_one(uint32_t event, const vrf_entry_t *e, const vrf_af_state_t *af, const vrf_rt_t *rt,
                          uint8_t rt_type)
{
    if (e && af && rt)
    {
        notify_all(event, e, af->afi, af, rt, 1, rt_type);
    }
}

void vrf_pub_notify_af_import_rt_add(const vrf_entry_t *e, const vrf_af_state_t *af, const vrf_rt_t *rt,
                                     uint8_t rt_type)
{
    notify_rt_one(VRF_EVENT_AF_IMPORT_RT_ADD, e, af, rt, rt_type);
}

void vrf_pub_notify_af_import_rt_del(const vrf_entry_t *e, const vrf_af_state_t *af, const vrf_rt_t *rt,
                                     uint8_t rt_type)
{
    notify_rt_one(VRF_EVENT_AF_IMPORT_RT_DEL, e, af, rt, rt_type);
}

void vrf_pub_notify_af_export_rt_add(const vrf_entry_t *e, const vrf_af_state_t *af, const vrf_rt_t *rt,
                                     uint8_t rt_type)
{
    notify_rt_one(VRF_EVENT_AF_EXPORT_RT_ADD, e, af, rt, rt_type);
}

void vrf_pub_notify_af_export_rt_del(const vrf_entry_t *e, const vrf_af_state_t *af, const vrf_rt_t *rt,
                                     uint8_t rt_type)
{
    notify_rt_one(VRF_EVENT_AF_EXPORT_RT_DEL, e, af, rt, rt_type);
}

// ============================================================================
// 全量回放（订阅 + REPLAY）
// ============================================================================

static void replay_one(uint32_t module_id, uint32_t event_mask, uint32_t af_mask, uint32_t event, const vrf_entry_t *e,
                       uint16_t afi, const vrf_af_state_t *af, const vrf_rt_t *rts, uint16_t rt_count, uint8_t rt_type)
{
    if (!subscriber_match(event_mask, af_mask, event, afi))
    {
        return;
    }
    size_t sz = 0;
    vrf_event_msg_t *src = build_event(e, event, afi, af, rts, rt_count, rt_type, &sz);
    if (!src)
    {
        return;
    }
    send_event_to(module_id, src, sz);
    g_free(src);
}

static void replay_af(uint32_t module_id, uint32_t event_mask, uint32_t af_mask, const vrf_entry_t *e,
                      const vrf_af_state_t *af)
{
    replay_one(module_id, event_mask, af_mask, VRF_EVENT_AF_ENABLE, e, af->afi, af, NULL, 0, VRF_RT_TYPE_VPN);
    if (af->has_rd)
    {
        replay_one(module_id, event_mask, af_mask, VRF_EVENT_AF_RD_ADD, e, af->afi, af, NULL, 0, VRF_RT_TYPE_VPN);
    }
    if (af->apply_label_mode != VRF_APPLY_LABEL_PER_VRF)
    {
        replay_one(module_id, event_mask, af_mask, VRF_EVENT_AF_APPLY_LABEL, e, af->afi, af, NULL, 0,
                   VRF_RT_TYPE_VPN);
    }
    if (af->import_rts && af->import_rts->len > 0)
    {
        for (guint i = 0; i < af->import_rts->len; i++)
        {
            const vrf_rt_t *rt = &g_array_index(af->import_rts, vrf_rt_t, i);
            replay_one(module_id, event_mask, af_mask, VRF_EVENT_AF_IMPORT_RT_ADD, e, af->afi, af, rt, 1,
                       VRF_RT_TYPE_VPN);
        }
    }
    if (af->export_rts && af->export_rts->len > 0)
    {
        for (guint i = 0; i < af->export_rts->len; i++)
        {
            const vrf_rt_t *rt = &g_array_index(af->export_rts, vrf_rt_t, i);
            replay_one(module_id, event_mask, af_mask, VRF_EVENT_AF_EXPORT_RT_ADD, e, af->afi, af, rt, 1,
                       VRF_RT_TYPE_VPN);
        }
    }
    if (af->evpn_import_rts && af->evpn_import_rts->len > 0)
    {
        for (guint i = 0; i < af->evpn_import_rts->len; i++)
        {
            const vrf_rt_t *rt = &g_array_index(af->evpn_import_rts, vrf_rt_t, i);
            replay_one(module_id, event_mask, af_mask, VRF_EVENT_AF_IMPORT_RT_ADD, e, af->afi, af, rt, 1,
                       VRF_RT_TYPE_EVPN);
        }
    }
    if (af->evpn_export_rts && af->evpn_export_rts->len > 0)
    {
        for (guint i = 0; i < af->evpn_export_rts->len; i++)
        {
            const vrf_rt_t *rt = &g_array_index(af->evpn_export_rts, vrf_rt_t, i);
            replay_one(module_id, event_mask, af_mask, VRF_EVENT_AF_EXPORT_RT_ADD, e, af->afi, af, rt, 1,
                       VRF_RT_TYPE_EVPN);
        }
    }
}

/* 单独投递一条平滑同步标记事件（不受 af_mask 限制：标记事件无地址族维度）。
 * 直接走 send_event_to，绕过 subscriber_match 中的事件位检查——
 * smoothstart/smoothend 是 REPLAY 路径的强制框架，订阅方无需显式 opt-in。 */
static void send_smooth_marker(uint32_t module_id, uint32_t event)
{
    size_t sz = 0;
    vrf_event_msg_t *src = build_event(NULL, event, 0, NULL, NULL, 0, VRF_RT_TYPE_VPN, &sz);
    if (!src)
    {
        return;
    }
    send_event_to(module_id, src, sz);
    g_free(src);
}

static void replay_full(uint32_t module_id, uint32_t af_mask, uint32_t event_mask)
{
    send_smooth_marker(module_id, VRF_EVENT_SMOOTHSTART);

    vrf_table_t *t = vrf_worker_table();
    if (t && t->by_id)
    {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer val = NULL;
        g_hash_table_iter_init(&iter, t->by_id);
        while (g_hash_table_iter_next(&iter, &key, &val))
        {
            (void)key;
            const vrf_entry_t *e = (const vrf_entry_t *)val;
            replay_one(module_id, event_mask, af_mask, VRF_EVENT_VRF_ADD, e, 0, NULL, NULL, 0, VRF_RT_TYPE_VPN);
            if (e->os_state != VRF_OS_STATE_UNKNOWN)
            {
                replay_one(module_id, event_mask, af_mask, VRF_EVENT_VRF_STATE, e, 0, NULL, NULL, 0,
                           VRF_RT_TYPE_VPN);
            }
            if (!e->afs)
            {
                continue;
            }
            GHashTableIter af_iter;
            gpointer af_key = NULL;
            gpointer af_val = NULL;
            g_hash_table_iter_init(&af_iter, e->afs);
            while (g_hash_table_iter_next(&af_iter, &af_key, &af_val))
            {
                (void)af_key;
                replay_af(module_id, event_mask, af_mask, e, (const vrf_af_state_t *)af_val);
            }
        }
    }

    send_smooth_marker(module_id, VRF_EVENT_SMOOTHEND);
}

// ============================================================================
// 订阅消息处理
// ============================================================================

static void send_ack(dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_context_t *ctx = vrf_worker_ipc_ctx();
    if (!ctx || !msg)
    {
        return;
    }
    vrf_msg_ack_t *ack = g_malloc0(sizeof(*ack));
    ack->result = result;
    dev_ipc_message_t *resp = dev_ipc_message_create(VRF_MSG_TYPE_ACK, DEV_MODULE_ID_VRF, msg->src_module_id,
                                                     msg->request_id, ack, sizeof(*ack), g_free);
    if (!resp)
    {
        g_free(ack);
        return;
    }
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
}

void vrf_pub_handle_subscribe(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    if (!msg->payload || msg->payload_len < sizeof(vrf_subscribe_req_t))
    {
        send_ack(msg, ERRCODE_FAIL);
        dev_ipc_message_free(msg);
        return;
    }
    const vrf_subscribe_req_t *req = (const vrf_subscribe_req_t *)msg->payload;
    if (req->af_mask == 0 || req->event_mask == 0)
    {
        send_ack(msg, ERRCODE_FAIL);
        dev_ipc_message_free(msg);
        return;
    }

    GList **plist = vrf_worker_subscribers_ptr();
    vrf_subscriber_t *sub = NULL;
    for (GList *l = *plist; l; l = l->next)
    {
        vrf_subscriber_t *cur = (vrf_subscriber_t *)l->data;
        if (cur->module_id == msg->src_module_id)
        {
            sub = cur;
            break;
        }
    }
    if (!sub)
    {
        sub = g_malloc0(sizeof(*sub));
        sub->module_id = msg->src_module_id;
        *plist = g_list_append(*plist, sub);
    }
    sub->af_mask = req->af_mask;
    sub->event_mask = req->event_mask;

    LOG_INFO("VRF: module 0x%08X subscribed: af=0x%08X event=0x%08X", msg->src_module_id, req->af_mask,
             req->event_mask);

    if ((req->flags & VRF_SUBSCRIBE_FLAG_REPLAY) != 0)
    {
        if (vrf_worker_is_restore_done())
        {
            replay_full(msg->src_module_id, req->af_mask, req->event_mask);
        }
        else
        {
            /* DB 恢复未完成：先记住要补发，等 restore 完成后由 flush 统一推送 */
            sub->pending_replay = 1;
            LOG_INFO("VRF: defer REPLAY to module 0x%08X until db restore done", msg->src_module_id);
        }
    }
    send_ack(msg, ERRCODE_SUCCESS);
    dev_ipc_message_free(msg);
}

void vrf_pub_flush_pending_replays(void)
{
    GList **plist = vrf_worker_subscribers_ptr();
    for (GList *l = *plist; l; l = l->next)
    {
        vrf_subscriber_t *sub = (vrf_subscriber_t *)l->data;
        if (!sub || !sub->pending_replay)
        {
            continue;
        }
        LOG_INFO("VRF: flushing deferred REPLAY to module 0x%08X (af=0x%08X event=0x%08X)", sub->module_id,
                 sub->af_mask, sub->event_mask);
        replay_full(sub->module_id, sub->af_mask, sub->event_mask);
        sub->pending_replay = 0;
    }
}

void vrf_pub_handle_unsubscribe(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    if (!msg->payload || msg->payload_len < sizeof(vrf_subscribe_req_t))
    {
        send_ack(msg, ERRCODE_FAIL);
        dev_ipc_message_free(msg);
        return;
    }
    const vrf_subscribe_req_t *req = (const vrf_subscribe_req_t *)msg->payload;

    GList **plist = vrf_worker_subscribers_ptr();
    GList *l = *plist;
    while (l)
    {
        GList *next = l->next;
        vrf_subscriber_t *sub = (vrf_subscriber_t *)l->data;
        if (sub->module_id == msg->src_module_id)
        {
            gboolean clear_all = (req->af_mask == 0 && req->event_mask == 0);
            if (clear_all)
            {
                *plist = g_list_delete_link(*plist, l);
                g_free(sub);
            }
            else
            {
                sub->af_mask &= ~req->af_mask;
                sub->event_mask &= ~req->event_mask;
                if (sub->af_mask == 0 || sub->event_mask == 0)
                {
                    *plist = g_list_delete_link(*plist, l);
                    g_free(sub);
                }
            }
        }
        l = next;
    }

    send_ack(msg, ERRCODE_SUCCESS);
    dev_ipc_message_free(msg);
}
