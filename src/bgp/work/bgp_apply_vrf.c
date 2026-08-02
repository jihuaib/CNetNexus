/**
 * @file   bgp_apply_vrf.c
 * @brief  BGP 应用 VRF 事件实现
 * @author jhb
 * @date   2026/05/02
 */
#include "bgp_apply_vrf.h"

#include <stddef.h>
#include <string.h>

#include "bgp_instance.h"
#include "bgp_protocol.h"
#include "bgp_rd.h"
#include "bgp_relay.h"
#include "bgp_vrf.h"
#include "bgp_vrf_export.h"
#include "bgp_vrf_import.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "vrf.h"

/* bgp_rd_t 与 vrf_rd_t 二进制同构（8B raw） */
_Static_assert(sizeof(bgp_rd_t) == sizeof(vrf_rd_t), "bgp_rd_t / vrf_rd_t size mismatch");

#define BGP_VRF_CLEANUP_AF_V4 (1u << 0)
#define BGP_VRF_CLEANUP_AF_V6 (1u << 1)
#define BGP_VRF_CLEANUP_RETRY_BASE_USEC ((gint64)G_USEC_PER_SEC)
#define BGP_VRF_CLEANUP_RETRY_MAX_USEC (30 * (gint64)G_USEC_PER_SEC)
#define BGP_VRF_CLEANUP_RETRY_MAX_EXP 5u
#define BGP_VRF_CLEANUP_RETRY_BATCH 1u

static bgp_protocol_t *bgp_proto(void)
{
    return g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
}

static bgp_afi_t map_afi(uint16_t afi)
{
    if (afi == VRF_AFI_IPV4)
    {
        return BGP_AFI_IPV4;
    }
    if (afi == VRF_AFI_IPV6)
    {
        return BGP_AFI_IPV6;
    }
    return (bgp_afi_t)afi;
}

static uint8_t cleanup_af_bit(bgp_afi_t afi)
{
    return (afi == BGP_AFI_IPV4) ? BGP_VRF_CLEANUP_AF_V4 : (afi == BGP_AFI_IPV6) ? BGP_VRF_CLEANUP_AF_V6 : 0u;
}

static void cleanup_retry_reset(bgp_vrf_t *vrf)
{
    if (!vrf)
    {
        return;
    }
    vrf->srv6_cleanup_pending_mask = 0u;
    vrf->srv6_cleanup_delete_af_mask = 0u;
    vrf->srv6_cleanup_retry_exp = 0u;
    vrf->srv6_cleanup_delete_vrf = FALSE;
    vrf->srv6_cleanup_retry_due_usec = 0;
}

static void cleanup_retry_arm(bgp_vrf_t *vrf)
{
    if (!vrf || vrf->srv6_cleanup_pending_mask == 0u)
    {
        return;
    }
    guint exp = MIN((guint)vrf->srv6_cleanup_retry_exp, (guint)BGP_VRF_CLEANUP_RETRY_MAX_EXP);
    gint64 delay = BGP_VRF_CLEANUP_RETRY_BASE_USEC << exp;
    delay = MIN(delay, BGP_VRF_CLEANUP_RETRY_MAX_USEC);
    vrf->srv6_cleanup_retry_due_usec = g_get_monotonic_time() + delay;
    if (vrf->srv6_cleanup_retry_exp < BGP_VRF_CLEANUP_RETRY_MAX_EXP)
    {
        vrf->srv6_cleanup_retry_exp++;
    }
    LOG_WARN("BGP: deferred SRv6 lifecycle cleanup vrf=%u mask=0x%x in %" G_GINT64_FORMAT " ms", vrf->vrf_id,
             vrf->srv6_cleanup_pending_mask, delay / 1000);
}

/** 保留 bgp_vrf 内的稳定 SID key，但记录原删除事务的后续动作。 */
static void cleanup_retry_schedule(bgp_vrf_t *vrf, uint8_t failed_mask, uint8_t delete_af_mask, gboolean delete_vrf)
{
    if (!vrf || failed_mask == 0u)
    {
        return;
    }
    if (delete_vrf)
    {
        /* 全 VRF 删除覆盖早先的单 AF 任务；已成功 release 的 AF
         * 不再试，只等 failed_mask 收敛后统一销毁 VRF。 */
        vrf->srv6_cleanup_pending_mask = failed_mask;
        vrf->srv6_cleanup_delete_af_mask = 0u;
        vrf->srv6_cleanup_delete_vrf = TRUE;
    }
    else
    {
        vrf->srv6_cleanup_pending_mask |= failed_mask;
        vrf->srv6_cleanup_delete_af_mask |= (delete_af_mask & failed_mask);
    }
    if (vrf->srv6_cleanup_retry_due_usec == 0)
    {
        cleanup_retry_arm(vrf);
    }
}

/** VRF/AF 重建使旧删除任务过时。清空的 SID cache 不直接复用；
 * 重建事件会先按同一稳定 key 幂等 alloc 确认，再执行 route backfill。 */
static void cleanup_retry_cancel_all(uint32_t vrf_id)
{
    bgp_protocol_t *proto = bgp_proto();
    bgp_vrf_t *vrf = proto ? bgp_protocol_get_vrf(proto, vrf_id) : NULL;
    if (!vrf || (vrf->srv6_cleanup_pending_mask == 0u && !vrf->srv6_cleanup_delete_vrf))
    {
        return;
    }
    LOG_INFO("BGP: canceled stale SRv6 VRF cleanup after VRF recreation vrf=%u", vrf_id);
    cleanup_retry_reset(vrf);
}

static void cleanup_retry_cancel_af(uint32_t vrf_id, bgp_afi_t afi)
{
    bgp_protocol_t *proto = bgp_proto();
    bgp_vrf_t *vrf = proto ? bgp_protocol_get_vrf(proto, vrf_id) : NULL;
    uint8_t bit = cleanup_af_bit(afi);
    if (!vrf || bit == 0u)
    {
        return;
    }
    if (vrf->srv6_cleanup_delete_vrf)
    {
        /* AF 重新 enable 表明同 ID VRF 已重建，整个旧 VRF_DEL 任务失效。 */
        cleanup_retry_cancel_all(vrf_id);
        return;
    }
    if ((vrf->srv6_cleanup_pending_mask & bit) == 0u)
    {
        return;
    }
    vrf->srv6_cleanup_pending_mask &= (uint8_t)~bit;
    vrf->srv6_cleanup_delete_af_mask &= (uint8_t)~bit;
    LOG_INFO("BGP: canceled stale SRv6 AF cleanup after AF recreation vrf=%u afi=%u", vrf_id, (unsigned)afi);
    if (vrf->srv6_cleanup_pending_mask == 0u)
    {
        cleanup_retry_reset(vrf);
    }
}

static bgp_instance_t *public_export_inst(bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_protocol_t *proto = bgp_proto();
    bgp_vrf_t *pub = proto ? bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID) : NULL;
    return (pub && pub->inst_hash) ? g_hash_table_lookup(pub->inst_hash, bgp_inst_hash_key(afi, safi)) : NULL;
}

static int drain_export_inst(bgp_instance_t *inst)
{
    if (inst)
    {
        bgp_instance_drain_pending(inst);
    }
    return bgp_instance_pending_count(inst) == 0u ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

/** VRF/AF 生命周期拆除：先撤 public VPN/EVPN 广告并 drain，再释放该 AF SID。 */
static int quiesce_vrf_af(bgp_vrf_t *vrf, bgp_afi_t afi)
{
    if (!vrf || !vrf->inst_hash || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return ERRCODE_SUCCESS;
    }
    bgp_instance_t *src = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, BGP_SAFI_UNICAST));
    bgp_vrf_export_purge_source_inst(src);
    if (drain_export_inst(public_export_inst(afi, BGP_SAFI_VPN_UNICAST)) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (afi == BGP_AFI_IPV4)
    {
        if (drain_export_inst(public_export_inst(BGP_AFI_L2VPN, BGP_SAFI_EVPN)) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }
    return bgp_vrf_export_release_srv6_sid(vrf, afi);
}

static void on_vrf_del(uint32_t vrf_id)
{
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || !proto->vrf_hash || vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
    if (!vrf)
    {
        return;
    }
    uint8_t failed_mask = 0u;
    if (quiesce_vrf_af(vrf, BGP_AFI_IPV4) != ERRCODE_SUCCESS)
    {
        failed_mask |= BGP_VRF_CLEANUP_AF_V4;
    }
    if (quiesce_vrf_af(vrf, BGP_AFI_IPV6) != ERRCODE_SUCCESS)
    {
        failed_mask |= BGP_VRF_CLEANUP_AF_V6;
    }
    if (failed_mask != 0u)
    {
        LOG_ERROR("BGP: failed to release one or more SRv6 SIDs for deleted VRF %u", vrf_id);
        cleanup_retry_schedule(vrf, failed_mask, 0u, TRUE);
        return;
    }
    cleanup_retry_reset(vrf);
    /* 通过 hash 移除将触发 bgp_vrf_destroy（hash value destroy notify） */
    g_hash_table_remove(proto->vrf_hash, &vrf_id);
    LOG_INFO("BGP: removed bgp_vrf for VRF %u (driven by VRF_EVENT_VRF_DEL)", vrf_id);
}

static void delete_vrf_af(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi, const char *reason)
{
    if (vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return;
    }

    bgp_protocol_t *proto = bgp_proto();
    if (!proto)
    {
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
    if (!vrf)
    {
        return;
    }
    bgp_vrf_del_instance(vrf, afi, safi);
    LOG_INFO("BGP: removed VRF %u afi=%u safi=%u (%s)", vrf_id, (unsigned)afi, (unsigned)safi,
             reason ? reason : "VRF event");
}

static void on_af_disable(uint32_t vrf_id, uint16_t afi)
{
    bgp_afi_t bafi = map_afi(afi);
    uint8_t bit = cleanup_af_bit(bafi);
    if (bit == 0u)
    {
        return;
    }
    bgp_protocol_t *proto = bgp_proto();
    bgp_vrf_t *vrf = proto ? bgp_protocol_get_vrf(proto, vrf_id) : NULL;
    if (vrf && quiesce_vrf_af(vrf, bafi) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: failed to release SRv6 SID for disabled VRF %u afi=%u", vrf_id, (unsigned)bafi);
        cleanup_retry_schedule(vrf, bit, bit, FALSE);
        return;
    }
    if (vrf)
    {
        vrf->srv6_cleanup_pending_mask &= (uint8_t)~bit;
        vrf->srv6_cleanup_delete_af_mask &= (uint8_t)~bit;
    }
    delete_vrf_af(vrf_id, bafi, BGP_SAFI_UNICAST, "driven by VRF_EVENT_AF_DISABLE");
}

static void on_af_rd_add(uint32_t vrf_id, uint16_t afi, const vrf_rd_t *rd)
{
    if (vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return; /* 公网 VRF 永远使用全 0 RD entry */
    }

    bgp_afi_t bafi = map_afi(afi);
    bgp_safi_t bsafi = BGP_SAFI_UNICAST;
    cleanup_retry_cancel_af(vrf_id, bafi);

    bgp_protocol_t *proto = bgp_proto();
    if (!proto)
    {
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
    if (!vrf)
    {
        /* BGP 还没 enable 该 VRF，事件先缓存即可，待 enable 时从 vrf_api_cache 取 */
        return;
    }

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(bafi, bsafi));
    if (!inst)
    {
        return;
    }

    bgp_rd_t bgp_rd;
    memcpy(bgp_rd.bytes, rd->bytes, sizeof(bgp_rd.bytes));
    (void)bgp_protocol_ensure_rd_entry(proto, inst, &bgp_rd);
    LOG_INFO("BGP: VRF %u afi=%u safi=%u RD ensured", vrf_id, afi, (unsigned)bsafi);

    /* A canceled cleanup may already have released the LocalSID and cleared
     * its volatile cache.  Repair the stable locator key before any route is
     * queued back to a SID update-group. */
    if (inst->srv6_locator[0] != '\0' && bgp_vrf_export_prepare_srv6_sid(inst) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: failed to restore SRv6 SID before RD backfill vrf=%u afi=%u locator=%s", vrf_id, (unsigned)bafi,
                  inst->srv6_locator);
    }

    /* RD 晚于 VPN AF 使能时，补灌对应 IPv4/IPv6 unicast。 */
    bgp_vrf_export_backfill_vrf(vrf_id);
}

static void on_af_rd_del(uint32_t vrf_id, uint16_t afi)
{
    bgp_afi_t bafi = map_afi(afi);
    uint8_t bit = cleanup_af_bit(bafi);
    if (bit == 0u)
    {
        return;
    }
    bgp_protocol_t *proto = bgp_proto();
    bgp_vrf_t *vrf = proto ? bgp_protocol_get_vrf(proto, vrf_id) : NULL;
    if (vrf && quiesce_vrf_af(vrf, bafi) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: failed to release SRv6 SID after RD removal vrf=%u afi=%u", vrf_id, (unsigned)bafi);
        cleanup_retry_schedule(vrf, bit, bit, FALSE);
        return;
    }
    if (vrf)
    {
        vrf->srv6_cleanup_pending_mask &= (uint8_t)~bit;
        vrf->srv6_cleanup_delete_af_mask &= (uint8_t)~bit;
    }
    delete_vrf_af(vrf_id, bafi, BGP_SAFI_UNICAST, "driven by VRF_EVENT_AF_RD_DEL");
}

void bgp_apply_vrf_cleanup_retry_tick(void)
{
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || !proto->vrf_hash)
    {
        return;
    }
    gint64 now = g_get_monotonic_time();
    GArray *due_ids = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, proto->vrf_hash);
    while (due_ids->len < BGP_VRF_CLEANUP_RETRY_BATCH && g_hash_table_iter_next(&iter, &key, &value))
    {
        bgp_vrf_t *vrf = value;
        if (vrf && vrf->vrf_id != BGP_VRF_PUBLIC_ID && vrf->srv6_cleanup_pending_mask != 0u &&
            vrf->srv6_cleanup_retry_due_usec != 0 && now >= vrf->srv6_cleanup_retry_due_usec)
        {
            uint32_t id = vrf->vrf_id;
            g_array_append_val(due_ids, id);
        }
    }

    for (guint i = 0; i < due_ids->len; ++i)
    {
        uint32_t vrf_id = g_array_index(due_ids, uint32_t, i);
        bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
        if (!vrf)
        {
            continue;
        }
        vrf->srv6_cleanup_retry_due_usec = 0;
        const bgp_afi_t afis[] = {BGP_AFI_IPV4, BGP_AFI_IPV6};
        for (guint ai = 0; ai < G_N_ELEMENTS(afis); ++ai)
        {
            uint8_t bit = cleanup_af_bit(afis[ai]);
            if ((vrf->srv6_cleanup_pending_mask & bit) == 0u || quiesce_vrf_af(vrf, afis[ai]) != ERRCODE_SUCCESS)
            {
                continue;
            }
            vrf->srv6_cleanup_pending_mask &= (uint8_t)~bit;
            if (!vrf->srv6_cleanup_delete_vrf && (vrf->srv6_cleanup_delete_af_mask & bit) != 0u)
            {
                vrf->srv6_cleanup_delete_af_mask &= (uint8_t)~bit;
                delete_vrf_af(vrf_id, afis[ai], BGP_SAFI_UNICAST, "after deferred SRv6 SID release");
            }
        }

        if (vrf->srv6_cleanup_pending_mask == 0u)
        {
            if (vrf->srv6_cleanup_delete_vrf)
            {
                cleanup_retry_reset(vrf);
                g_hash_table_remove(proto->vrf_hash, &vrf_id);
                LOG_INFO("BGP: completed deferred VRF cleanup vrf=%u", vrf_id);
            }
            else
            {
                cleanup_retry_reset(vrf);
            }
        }
        else
        {
            cleanup_retry_arm(vrf);
        }
    }
    g_array_free(due_ids, TRUE);
}

void bgp_apply_vrf_purge_non_public(void)
{
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || !proto->vrf_hash)
    {
        return;
    }

    /* 先收集非 public vrf_id，避免在迭代过程中修改 hash table */
    GArray *vrf_ids = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&iter, proto->vrf_hash);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        uint32_t vid = *(uint32_t *)key;
        if (vid != BGP_VRF_PUBLIC_ID)
        {
            g_array_append_val(vrf_ids, vid);
        }
    }

    for (guint i = 0; i < vrf_ids->len; i++)
    {
        uint32_t vid = g_array_index(vrf_ids, uint32_t, i);
        bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vid);
        uint8_t failed_mask = 0u;
        if (vrf && quiesce_vrf_af(vrf, BGP_AFI_IPV4) != ERRCODE_SUCCESS)
        {
            failed_mask |= BGP_VRF_CLEANUP_AF_V4;
        }
        if (vrf && quiesce_vrf_af(vrf, BGP_AFI_IPV6) != ERRCODE_SUCCESS)
        {
            failed_mask |= BGP_VRF_CLEANUP_AF_V6;
        }
        if (failed_mask != 0u)
        {
            LOG_ERROR("BGP resync: failed to release one or more SRv6 SIDs for VRF %u", vid);
            cleanup_retry_schedule(vrf, failed_mask, 0u, TRUE);
            continue;
        }
        cleanup_retry_reset(vrf);
        g_hash_table_remove(proto->vrf_hash, &vid); /* 触发 bgp_vrf_destroy */
        LOG_INFO("BGP resync: purged bgp_vrf for VRF %u", vid);
    }
    g_array_free(vrf_ids, TRUE);
}

void bgp_apply_vrf_event(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < offsetof(vrf_event_msg_t, rts))
    {
        return;
    }
    const vrf_event_msg_t *evt = (const vrf_event_msg_t *)msg->payload;

    switch (evt->event)
    {
        case VRF_EVENT_VRF_DEL:
            bgp_vrf_import_purge_vrf(evt->vrf_id);
            on_vrf_del(evt->vrf_id);
            break;

        case VRF_EVENT_AF_DISABLE:
            /* VRF 只通知 AFI；BGP 侧映射为 unicast SAFI。 */
            if (evt->afi == VRF_AFI_IPV4 || evt->afi == VRF_AFI_IPV6)
            {
                bgp_vrf_import_purge_vrf_afi(evt->vrf_id, map_afi(evt->afi));
            }
            on_af_disable(evt->vrf_id, evt->afi);
            bgp_vrf_import_backfill_afi(map_afi(evt->afi));
            break;

        case VRF_EVENT_AF_RD_ADD:
            on_af_rd_add(evt->vrf_id, evt->afi, &evt->rd);
            break;

        case VRF_EVENT_AF_RD_DEL:
            on_af_rd_del(evt->vrf_id, evt->afi);
            break;

        case VRF_EVENT_AF_IMPORT_RT_ADD:
            /* VPNv4/VPNv6 的 IRT 必须按 AF 隔离，变更只重评对应 VPN AF。 */
            if ((evt->afi == VRF_AFI_IPV4 || evt->afi == VRF_AFI_IPV6) && evt->rt_type == VRF_RT_TYPE_VPN &&
                msg->payload_len >= offsetof(vrf_event_msg_t, rts) + sizeof(vrf_rt_t) && evt->rt_count >= 1)
            {
                bgp_afi_t afi = map_afi(evt->afi);
                bgp_vrf_import_irt_add(evt->vrf_id, afi, &evt->rts[0]);
                bgp_vrf_import_backfill_afi(afi);
                bgp_vrf_import_request_refresh_afi(afi);
                if (afi == BGP_AFI_IPV4)
                {
                    /* 本地交叉子系统当前只处理 IPv4 unicast。 */
                    bgp_vrf_import_local_backfill_target_vrf(evt->vrf_id);
                }
            }
            break;

        case VRF_EVENT_AF_IMPORT_RT_DEL:
            if ((evt->afi == VRF_AFI_IPV4 || evt->afi == VRF_AFI_IPV6) && evt->rt_type == VRF_RT_TYPE_VPN &&
                msg->payload_len >= offsetof(vrf_event_msg_t, rts) + sizeof(vrf_rt_t) && evt->rt_count >= 1)
            {
                bgp_afi_t afi = map_afi(evt->afi);
                bgp_vrf_import_irt_del(evt->vrf_id, afi, &evt->rts[0]);
                bgp_vrf_import_backfill_afi(afi);
                bgp_vrf_import_request_refresh_afi(afi);
                if (afi == BGP_AFI_IPV4)
                {
                    bgp_vrf_import_local_backfill_target_vrf(evt->vrf_id);
                }
            }
            break;

        case VRF_EVENT_AF_EXPORT_RT_ADD:
        case VRF_EVENT_AF_EXPORT_RT_DEL:
            /* export-RT 由 vrf_api_cache 持有。其变化会影响：
             * 1) 本地交叉泄漏的目标集合；
             * 2) 本 VRF unicast RIB 已有路由的 effective ERT；
             * 3) 已导出到 vpnv4 的本地路由属性(ERT)。 */
            if (evt->afi == VRF_AFI_IPV4)
            {
                if (evt->rt_type == VRF_RT_TYPE_VPN)
                {
                    (void)bgp_relay_vrf_export_attr_rebuild(evt->vrf_id, BGP_AFI_IPV4);
                    bgp_vrf_import_local_backfill_source_vrf(evt->vrf_id);
                }
                bgp_vrf_export_backfill_vrf(evt->vrf_id);
            }
            else if (evt->afi == VRF_AFI_IPV6)
            {
                (void)bgp_relay_vrf_export_attr_rebuild(evt->vrf_id, BGP_AFI_IPV6);
                bgp_vrf_export_backfill_vrf(evt->vrf_id);
            }
            break;

        case VRF_EVENT_VRF_ADD:
        {
            cleanup_retry_cancel_all(evt->vrf_id);
            bgp_protocol_t *proto = bgp_proto();
            bgp_vrf_t *vrf = proto ? bgp_protocol_get_vrf(proto, evt->vrf_id) : NULL;
            if (vrf && vrf->inst_hash)
            {
                const bgp_afi_t afis[] = {BGP_AFI_IPV4, BGP_AFI_IPV6};
                for (guint i = 0; i < G_N_ELEMENTS(afis); ++i)
                {
                    bgp_instance_t *inst =
                        g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afis[i], BGP_SAFI_UNICAST));
                    if (inst && inst->srv6_locator[0] != '\0' &&
                        bgp_vrf_export_prepare_srv6_sid(inst) != ERRCODE_SUCCESS)
                    {
                        LOG_ERROR("BGP: failed to restore SRv6 SID after VRF recreation vrf=%u afi=%u locator=%s",
                                  evt->vrf_id, (unsigned)afis[i], inst->srv6_locator);
                    }
                }
            }
            bgp_vrf_export_backfill_vrf(evt->vrf_id);
            break;
        }

        case VRF_EVENT_AF_ENABLE:
        {
            bgp_afi_t afi = map_afi(evt->afi);
            cleanup_retry_cancel_af(evt->vrf_id, afi);
            bgp_protocol_t *proto = bgp_proto();
            bgp_vrf_t *vrf = proto ? bgp_protocol_get_vrf(proto, evt->vrf_id) : NULL;
            bgp_instance_t *inst = vrf && vrf->inst_hash
                                       ? g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, BGP_SAFI_UNICAST))
                                       : NULL;
            if (inst && inst->srv6_locator[0] != '\0' && bgp_vrf_export_prepare_srv6_sid(inst) != ERRCODE_SUCCESS)
            {
                LOG_ERROR("BGP: failed to restore SRv6 SID after AF recreation vrf=%u afi=%u locator=%s", evt->vrf_id,
                          (unsigned)afi, inst->srv6_locator);
            }
            /* RD_ADD 通常紧随其后；先 repair SID，再尝试 backfill，无 RD 时为 no-op。 */
            bgp_vrf_export_backfill_vrf(evt->vrf_id);
            break;
        }

        case VRF_EVENT_VRF_STATE:
            /* 当前阶段仅由 vrf_api_cache 持有；BGP 内部使用从缓存按需读取 */
            break;

        default:
            break;
    }
}
