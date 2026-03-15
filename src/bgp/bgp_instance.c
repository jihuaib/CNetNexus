/**
 * @file   bgp_instance.c
 * @brief  BGP 地址族实例生命周期实现
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_instance.h"

#include <glib.h>

#include "bgp_calc.h"
#include "bgp_pub.h"
#include "bgp_rib.h"
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
    inst->publist = bgp_publist_create(inst);
    inst->bestlist = bgp_bestlist_create();
    return inst;
}

void bgp_instance_destroy(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }
    bgp_publist_destroy(inst->publist);
    inst->publist = NULL;
    bgp_bestlist_destroy(inst->bestlist);
    inst->bestlist = NULL;
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
