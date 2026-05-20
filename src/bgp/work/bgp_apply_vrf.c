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
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "log.h"
#include "vrf.h"

/* bgp_rd_t 与 vrf_rd_t 二进制同构（8B raw） */
_Static_assert(sizeof(bgp_rd_t) == sizeof(vrf_rd_t), "bgp_rd_t / vrf_rd_t size mismatch");

static bgp_protocol_t *bgp_proto(void)
{
    return g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
}

static bgp_safi_t map_safi(uint8_t safi)
{
    return (safi == VRF_SAFI_UNICAST) ? BGP_SAFI_UNICAST : (bgp_safi_t)safi;
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

static void on_af_disable(uint32_t vrf_id, uint16_t afi, uint8_t safi)
{
    delete_vrf_af(vrf_id, map_afi(afi), map_safi(safi), "driven by VRF_EVENT_AF_DISABLE");
}

static void on_af_rd_add(uint32_t vrf_id, uint16_t afi, uint8_t safi, const vrf_rd_t *rd)
{
    if (vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return; /* 公网 VRF 永远使用全 0 RD entry */
    }

    bgp_afi_t bafi = map_afi(afi);
    bgp_safi_t bsafi = map_safi(safi);

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
    LOG_INFO("BGP: VRF %u afi=%u safi=%u RD ensured", vrf_id, afi, safi);
}

static void on_af_rd_del(uint32_t vrf_id, uint16_t afi, uint8_t safi)
{
    delete_vrf_af(vrf_id, map_afi(afi), map_safi(safi), "driven by VRF_EVENT_AF_RD_DEL");
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
            on_vrf_del(evt->vrf_id);
            break;

        case VRF_EVENT_AF_DISABLE:
            on_af_disable(evt->vrf_id, evt->afi, evt->safi);
            break;

        case VRF_EVENT_AF_RD_ADD:
            on_af_rd_add(evt->vrf_id, evt->afi, evt->safi, &evt->rd);
            break;

        case VRF_EVENT_AF_RD_DEL:
            on_af_rd_del(evt->vrf_id, evt->afi, evt->safi);
            break;

        case VRF_EVENT_VRF_ADD:
        case VRF_EVENT_VRF_STATE:
        case VRF_EVENT_AF_ENABLE:
        case VRF_EVENT_AF_IMPORT_RT_ADD:
        case VRF_EVENT_AF_IMPORT_RT_DEL:
        case VRF_EVENT_AF_EXPORT_RT_ADD:
        case VRF_EVENT_AF_EXPORT_RT_DEL:
            /* 当前阶段仅由 vrf_api_cache 持有；BGP 内部使用从缓存按需读取 */
            break;

        default:
            break;
    }
}
