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

static void on_af_rd_change(uint32_t vrf_id, uint16_t afi, uint8_t safi, uint8_t has_rd, const vrf_rd_t *rd)
{
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return; /* 公网 VRF 永远使用全 0 RD entry */
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
    if (!vrf)
    {
        /* BGP 还没 enable 该 VRF，事件先缓存即可，待 enable 时从 vrf_api_cache 取 */
        return;
    }

    bgp_afi_t bafi = map_afi(afi);
    bgp_safi_t bsafi = map_safi(safi);
    bgp_instance_t *inst = bgp_vrf_get_or_create_instance(vrf, bafi, bsafi);
    if (!inst)
    {
        return;
    }

    if (!has_rd)
    {
        /* 清除 RD：暂仅记录；保留旧 entry 直至下条 RD 写入或 VRF 删除 */
        LOG_INFO("BGP: VRF %u afi=%u safi=%u RD cleared", vrf_id, afi, safi);
        return;
    }

    bgp_rd_t bgp_rd;
    memcpy(bgp_rd.bytes, rd->bytes, sizeof(bgp_rd.bytes));
    (void)bgp_protocol_ensure_rd_entry(proto, inst, &bgp_rd);
    LOG_INFO("BGP: VRF %u afi=%u safi=%u RD ensured", vrf_id, afi, safi);
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

        case VRF_EVENT_AF_RD_CHANGE:
            on_af_rd_change(evt->vrf_id, evt->afi, evt->safi, evt->has_rd, &evt->rd);
            break;

        case VRF_EVENT_VRF_ADD:
        case VRF_EVENT_VRF_STATE:
        case VRF_EVENT_AF_ENABLE:
        case VRF_EVENT_AF_DISABLE:
        case VRF_EVENT_AF_IMPORT_RT_CHG:
        case VRF_EVENT_AF_EXPORT_RT_CHG:
            /* 当前阶段仅由 vrf_api_cache 持有；BGP 内部使用从缓存按需读取 */
            break;

        default:
            break;
    }
}
