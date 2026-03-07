/**
 * @file   bgp_instance.c
 * @brief  BGP 地址族实例生命周期实现
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_instance.h"

#include <glib.h>

#include "log.h"

bgp_instance_t *bgp_instance_create(bgp_afi_t afi, bgp_safi_t safi, bgp_vrf_t *vrf)
{
    bgp_instance_t *inst = g_malloc0(sizeof(bgp_instance_t));
    inst->afi = afi;
    inst->safi = safi;
    inst->vrf = vrf;
    /* key: gchar*(addr_str)，value: bgp_peer_t*（负责销毁） */
    inst->peer_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)bgp_peer_destroy);
    return inst;
}

void bgp_instance_destroy(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }
    if (inst->peer_hash)
    {
        g_hash_table_destroy(inst->peer_hash);
        inst->peer_hash = NULL;
    }
    g_free(inst);
}
