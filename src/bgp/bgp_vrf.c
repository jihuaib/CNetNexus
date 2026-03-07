/**
 * @file   bgp_vrf.c
 * @brief  BGP VRF 层实现（聚合会话和地址族实例管理）
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_vrf.h"

#include <stdio.h>
#include <string.h>

#include "log.h"

// ============================================================================
// VRF 生命周期
// ============================================================================

bgp_vrf_t *bgp_vrf_create(uint32_t vrf_id)
{
    bgp_vrf_t *vrf = g_malloc0(sizeof(bgp_vrf_t));
    vrf->vrf_id = vrf_id;
    vrf->keepalive = BGP_TIMER_DEFAULT_KEEPALIVE;
    vrf->hold_time = BGP_TIMER_DEFAULT_HOLD;
    vrf->connect_retry = BGP_TIMER_DEFAULT_CONNECT_RETRY;
    /* sess_hash: key = addr_str(gchar*)，value = bgp_session_t*（负责销毁） */
    vrf->sess_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)bgp_session_destroy);
    /* inst_hash: key = bgp_inst_hash_key(afi,safi)（gpointer 直接值），value = bgp_instance_t*（负责销毁） */
    vrf->inst_hash = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)bgp_instance_destroy);
    LOG_INFO("BGP VRF 已创建: id=%u", vrf_id);
    return vrf;
}

void bgp_vrf_destroy(bgp_vrf_t *vrf)
{
    if (!vrf)
    {
        return;
    }
    LOG_INFO("BGP VRF 已销毁: id=%u", vrf->vrf_id);
    /* 先销毁 inst_hash（释放 bgp_peer_t），再销毁 sess_hash（仅释放借用引用链表节点） */
    if (vrf->inst_hash)
    {
        g_hash_table_destroy(vrf->inst_hash);
        vrf->inst_hash = NULL;
    }
    if (vrf->sess_hash)
    {
        g_hash_table_destroy(vrf->sess_hash);
        vrf->sess_hash = NULL;
    }
    g_free(vrf);
}

// ============================================================================
// 会话操作
// ============================================================================

void bgp_vrf_add_session(bgp_vrf_t *vrf, bgp_session_t *session)
{
    if (!vrf || !session)
    {
        return;
    }
    char addr_key[64];
    net_addr_to_str(&session->neighbor_addr, addr_key, sizeof(addr_key));
    g_hash_table_insert(vrf->sess_hash, g_strdup(addr_key), session);

    LOG_INFO("BGP session 已加入 VRF %u: neighbor=%s AS=%u", vrf->vrf_id, addr_key, session->remote_as);
}

void bgp_vrf_del_session(bgp_vrf_t *vrf, const net_addr_t *addr)
{
    if (!vrf || !addr)
    {
        return;
    }
    char addr_key[64];
    net_addr_to_str(addr, addr_key, sizeof(addr_key));
    /* g_hash_table_remove 会触发 bgp_session_destroy */
    g_hash_table_remove(vrf->sess_hash, addr_key);
    LOG_INFO("BGP session 已从 VRF %u 删除: neighbor=%s", vrf->vrf_id, addr_key);
}

bgp_session_t *bgp_vrf_find_session(bgp_vrf_t *vrf, const net_addr_t *addr)
{
    if (!vrf || !addr)
    {
        return NULL;
    }
    char addr_key[64];
    net_addr_to_str(addr, addr_key, sizeof(addr_key));
    return g_hash_table_lookup(vrf->sess_hash, addr_key);
}

// ============================================================================
// 地址族邻居操作
// ============================================================================

int bgp_vrf_af_enable_neighbor(bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi, const net_addr_t *addr)
{
    if (!vrf || !addr)
    {
        return -1;
    }

    /* 查找对应的 session */
    bgp_session_t *sess = bgp_vrf_find_session(vrf, addr);
    if (!sess)
    {
        char addr_str[64];
        net_addr_to_str(addr, addr_str, sizeof(addr_str));
        LOG_ERROR("BGP: 邻居 %s 的 session 不存在，无法使能 AF", addr_str);
        return -1;
    }

    /* 获取或创建地址族实例 */
    gpointer inst_key = bgp_inst_hash_key(afi, safi);

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, inst_key);
    if (!inst)
    {
        inst = bgp_instance_create(afi, safi, vrf);
        g_hash_table_insert(vrf->inst_hash, inst_key, inst);
        LOG_INFO("BGP: 创建地址族实例 afi=%u safi=%u (VRF %u)", (unsigned)afi, (unsigned)safi, vrf->vrf_id);
    }

    /* 若该邻居在此实例下已使能，直接返回 */
    char addr_key[64];
    net_addr_to_str(addr, addr_key, sizeof(addr_key));

    if (g_hash_table_lookup(inst->peer_hash, addr_key))
    {
        LOG_INFO("BGP: 邻居 %s 在实例 afi=%u safi=%u 下已使能", addr_key, (unsigned)afi, (unsigned)safi);
        return 0;
    }

    /* 创建 per-AF peer，挂入 instance.peer_hash（inst 持有所有权） */
    bgp_peer_t *peer = bgp_peer_create(vrf, inst, addr);
    g_hash_table_insert(inst->peer_hash, g_strdup(addr_key), peer);

    /* 同时将借用引用加入 session->peer_list，便于通过 session 快速查询 */
    sess->peer_list = g_list_append(sess->peer_list, peer);

    LOG_INFO("BGP: 邻居 %s 已在实例 afi=%u safi=%u (VRF %u) 下使能", addr_key, (unsigned)afi, (unsigned)safi,
             vrf->vrf_id);
    return 0;
}

int bgp_vrf_af_disable_neighbor(bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi, const net_addr_t *addr)
{
    if (!vrf || !addr)
    {
        return -1;
    }

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi));
    if (!inst)
    {
        return 0; /* 实例不存在，视为成功 */
    }

    char addr_key[64];
    net_addr_to_str(addr, addr_key, sizeof(addr_key));

    bgp_peer_t *peer = g_hash_table_lookup(inst->peer_hash, addr_key);
    if (!peer)
    {
        return 0; /* 邻居未在该实例下使能 */
    }

    /* 先从 session->peer_list 移除借用引用（不销毁 peer） */
    bgp_session_t *sess = bgp_vrf_find_session(vrf, addr);
    if (sess)
    {
        sess->peer_list = g_list_remove(sess->peer_list, peer);
    }

    /* 再从 instance.peer_hash 中删除，触发 bgp_peer_destroy */
    g_hash_table_remove(inst->peer_hash, addr_key);

    LOG_INFO("BGP: 邻居 %s 已从实例 afi=%u safi=%u (VRF %u) 下停用", addr_key, (unsigned)afi, (unsigned)safi,
             vrf->vrf_id);
    return 0;
}

GList *bgp_vrf_get_session_peers(bgp_vrf_t *vrf, const net_addr_t *addr)
{
    if (!vrf || !addr)
    {
        return NULL;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, addr);
    if (!sess)
    {
        return NULL;
    }

    /* 返回浅拷贝，调用方负责 g_list_free，不可销毁元素 */
    return g_list_copy(sess->peer_list);
}

gboolean bgp_vrf_neighbor_has_any_af(bgp_vrf_t *vrf, const net_addr_t *addr)
{
    if (!vrf || !addr)
    {
        return FALSE;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, addr);
    return sess != NULL && sess->peer_list != NULL;
}

// ============================================================================
// 地址族实例操作
// ============================================================================

bgp_instance_t *bgp_vrf_get_or_create_instance(bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!vrf)
    {
        return NULL;
    }

    gpointer inst_key = bgp_inst_hash_key(afi, safi);

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, inst_key);
    if (!inst)
    {
        inst = bgp_instance_create(afi, safi, vrf);
        g_hash_table_insert(vrf->inst_hash, inst_key, inst);
        LOG_INFO("BGP: 创建地址族实例 afi=%u safi=%u (VRF %u)", (unsigned)afi, (unsigned)safi, vrf->vrf_id);
    }

    return inst;
}

void bgp_vrf_del_instance(bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!vrf)
    {
        return;
    }

    /* g_hash_table_remove 触发 bgp_instance_destroy（含所有 peer） */
    g_hash_table_remove(vrf->inst_hash, bgp_inst_hash_key(afi, safi));
    LOG_INFO("BGP: 删除地址族实例 afi=%u safi=%u (VRF %u)", (unsigned)afi, (unsigned)safi, vrf->vrf_id);
}
