/**
 * @file   bgp_instance.c
 * @brief  BGP 地址族实例生命周期实现
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_instance.h"

#include <glib.h>

#include "bgp_rib.h"
#include "bgp_work.h"
#include "log.h"
#include "net_addr.h"

bgp_instance_t *bgp_instance_create(bgp_afi_t afi, bgp_safi_t safi, bgp_vrf_t *vrf)
{
    bgp_instance_t *inst = g_malloc0(sizeof(bgp_instance_t));
    inst->afi = afi;
    inst->safi = safi;
    inst->vrf = vrf;
    /* key: net_addr_t*（堆分配，g_free 释放），value: bgp_peer_t*（负责销毁） */
    inst->peer_hash =
        g_hash_table_new_full(net_addr_hash, net_addr_hash_equal, g_free, (GDestroyNotify)bgp_peer_destroy);
    inst->rib = bgp_rib_create();
    inst->rib->inst = inst; /* 建立 RIB → instance 反向引用 */
    inst->calc_queue = bgp_calc_queue_create();
    inst->pub_queue = bgp_pub_queue_create();
    inst->route_flush_queue = bgp_route_flush_queue_create();
    inst->work_timerfd = -1;
    bgp_work_timer_start(inst, BGP_WORK_TIMER_INTERVAL_MS);
    return inst;
}

void bgp_instance_destroy(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }
    bgp_work_timer_stop(inst);
    bgp_calc_queue_destroy(inst->calc_queue, inst);
    inst->calc_queue = NULL;
    bgp_pub_queue_destroy(inst->pub_queue, inst);
    inst->pub_queue = NULL;
    bgp_route_flush_queue_destroy(inst->route_flush_queue, inst);
    inst->route_flush_queue = NULL;
    if (inst->peer_hash)
    {
        g_hash_table_destroy(inst->peer_hash);
        inst->peer_hash = NULL;
    }
    if (inst->rib)
    {
        bgp_rib_destroy(inst->rib);
        inst->rib = NULL;
    }
    g_free(inst);
}
