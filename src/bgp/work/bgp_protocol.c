/**
 * @file   bgp_protocol.c
 * @brief  BGP 全局协议结构管理（含 VRF 哈希表）
 * @author jhb
 * @date   2026/03/02
 */
#include "bgp_protocol.h"

#include <stdio.h>
#include <string.h>

#include "bgp_rd.h"
#include "bgp_relay.h"
#include "log.h"
#include "vrf.h"

// ============================================================================
// 协议生命周期
// ============================================================================

bgp_protocol_t *bgp_protocol_create(uint32_t as_number)
{
    bgp_protocol_t *proto = g_malloc0(sizeof(bgp_protocol_t));
    proto->as_number = as_number;
    /* vrf_hash: key = uint32_t*(vrf_id)，value = bgp_vrf_t*（负责销毁） */
    proto->vrf_hash = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, (GDestroyNotify)bgp_vrf_destroy);
    /* rd_hash: key 指向 entry->key（不另堆分配），value = bgp_rd_entry_t*（负责销毁） */
    proto->rd_hash = g_hash_table_new_full(bgp_rd_key_hash, bgp_rd_key_equal, NULL, bgp_rd_entry_destroy_notify());

    /* 自动创建 vrf_id=0 的默认公网 VRF */
    bgp_vrf_t *default_vrf = bgp_vrf_create(BGP_VRF_PUBLIC_ID);
    uint32_t *vrf_key = g_malloc(sizeof(uint32_t));
    *vrf_key = BGP_VRF_PUBLIC_ID;
    g_hash_table_insert(proto->vrf_hash, vrf_key, default_vrf);

    LOG_INFO("BGP protocol structure created: AS %u", as_number);
    return proto;
}

void bgp_protocol_destroy(bgp_protocol_t *proto)
{
    if (!proto)
    {
        return;
    }
    LOG_INFO("BGP protocol structure destroyed: AS %u", proto->as_number);
    /* 先清理 relay watch 与对应借用引用，避免借用计数未归零的路径节点在
     * RIB 销毁阶段被保留导致 ASAN 退出泄漏。 */
    bgp_relay_cleanup();
    /* 先销毁 vrf_hash（间接触发 instance 销毁，instance 会从 rd_hash 摘除自己的 entry），
     * 再销毁残留的 rd_hash（理论上应已为空） */
    if (proto->vrf_hash)
    {
        g_hash_table_destroy(proto->vrf_hash);
        proto->vrf_hash = NULL;
    }
    if (proto->rd_hash)
    {
        g_hash_table_destroy(proto->rd_hash);
        proto->rd_hash = NULL;
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

    /* 非公网 VRF 必须已在 VRF 模块创建（通过 vrf_api 缓存验证），否则拒绝隐式创建 */
    if (vrf_id != BGP_VRF_PUBLIC_ID && vrf_api_cache_lookup(vrf_id) == NULL)
    {
        LOG_WARN("BGP: refuse to create bgp_vrf for unknown VRF id=%u (not present in vrf_api cache)", vrf_id);
        return NULL;
    }

    vrf = bgp_vrf_create(vrf_id);
    uint32_t *key = g_malloc(sizeof(uint32_t));
    *key = vrf_id;
    g_hash_table_insert(proto->vrf_hash, key, vrf);
    return vrf;
}
