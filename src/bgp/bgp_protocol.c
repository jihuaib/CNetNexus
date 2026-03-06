/**
 * @file   bgp_protocol.c
 * @brief  BGP 全局协议结构管理（含 VRF 哈希表）
 * @author jhb
 * @date   2026/03/02
 */
#include "bgp_protocol.h"

#include <stdio.h>
#include <string.h>

#include "log.h"

// ============================================================================
// 协议生命周期
// ============================================================================

bgp_protocol_t *bgp_protocol_create(uint32_t as_number)
{
    bgp_protocol_t *proto = g_malloc0(sizeof(bgp_protocol_t));
    proto->as_number = as_number;
    /* vrf_hash: key = uint32_t*(vrf_id)，value = bgp_vrf_t*（负责销毁） */
    proto->vrf_hash = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, (GDestroyNotify)bgp_vrf_destroy);

    /* 自动创建 vrf_id=0 的默认公网 VRF */
    bgp_vrf_t *default_vrf = bgp_vrf_create(BGP_VRF_PUBLIC_ID);
    uint32_t *vrf_key = g_malloc(sizeof(uint32_t));
    *vrf_key = BGP_VRF_PUBLIC_ID;
    g_hash_table_insert(proto->vrf_hash, vrf_key, default_vrf);

    LOG_INFO("BGP 协议结构已创建: AS %u", as_number);
    return proto;
}

void bgp_protocol_destroy(bgp_protocol_t *proto)
{
    if (!proto)
    {
        return;
    }
    LOG_INFO("BGP 协议结构已销毁: AS %u", proto->as_number);
    if (proto->vrf_hash)
    {
        g_hash_table_destroy(proto->vrf_hash);
        proto->vrf_hash = NULL;
    }
    g_free(proto);
}

// ============================================================================
// VRF 查找
// ============================================================================

bgp_vrf_t *bgp_protocol_get_vrf(bgp_protocol_t *proto, uint32_t vrf_id)
{
    if (!proto || !proto->vrf_hash)
    {
        return NULL;
    }
    return g_hash_table_lookup(proto->vrf_hash, &vrf_id);
}

bgp_vrf_t *bgp_protocol_get_or_create_vrf(bgp_protocol_t *proto, uint32_t vrf_id)
{
    if (!proto)
    {
        return NULL;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
    if (vrf)
    {
        return vrf;
    }
    vrf = bgp_vrf_create(vrf_id);
    uint32_t *key = g_malloc(sizeof(uint32_t));
    *key = vrf_id;
    g_hash_table_insert(proto->vrf_hash, key, vrf);
    return vrf;
}
