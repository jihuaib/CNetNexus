/**
 * @file   bgp_vrf.c
 * @brief  BGP VRF 层实现（聚合会话和地址族实例管理）
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_vrf.h"

#include <stdio.h>
#include <string.h>

#include "bgp_rib.h"
#include "log.h"
#include "net_addr.h"

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
    /* sess_hash: key = net_addr_t*（堆分配，g_free 释放），value = bgp_session_t*（负责销毁） */
    vrf->sess_hash =
        g_hash_table_new_full(net_addr_hash, net_addr_hash_equal, g_free, (GDestroyNotify)bgp_session_destroy);
    /* inst_hash: key = bgp_inst_hash_key(afi,safi)（gpointer 直接值），value = bgp_instance_t*（负责销毁） */
    vrf->inst_hash = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)bgp_instance_destroy);
    LOG_INFO("BGP VRF created: id=%u", vrf_id);
    return vrf;
}

void bgp_vrf_destroy(bgp_vrf_t *vrf)
{
    if (!vrf)
    {
        return;
    }
    LOG_INFO("BGP VRF destroyed: id=%u", vrf->vrf_id);
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
    net_addr_t *key = g_malloc(sizeof(net_addr_t));
    *key = session->neighbor_addr;
    g_hash_table_insert(vrf->sess_hash, key, session);

    char addr_str[64];
    net_addr_to_str(&session->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP session joined VRF %u: neighbor=%s AS=%u", vrf->vrf_id, addr_str, session->remote_as);
}

void bgp_vrf_del_session(bgp_vrf_t *vrf, const net_addr_t *addr)
{
    if (!vrf || !addr)
    {
        return;
    }
    (void)bgp_vrf_purge_session_routes(vrf, addr);
    /* g_hash_table_remove 使用 net_addr_hash_equal 按值匹配，触发 bgp_session_destroy */
    g_hash_table_remove(vrf->sess_hash, addr);

    char addr_str[64];
    net_addr_to_str(addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP session removed from VRF %u: neighbor=%s", vrf->vrf_id, addr_str);
}

bgp_session_t *bgp_vrf_find_session(bgp_vrf_t *vrf, const net_addr_t *addr)
{
    if (!vrf || !addr)
    {
        return NULL;
    }
    return g_hash_table_lookup(vrf->sess_hash, addr);
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
        LOG_ERROR("BGP: Neighbor %s session does not exist, cannot enable AF", addr_str);
        return -1;
    }

    /* 获取或创建地址族实例 */
    gpointer inst_key = bgp_inst_hash_key(afi, safi);

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, inst_key);
    if (!inst)
    {
        inst = bgp_instance_create(afi, safi, vrf);
        g_hash_table_insert(vrf->inst_hash, inst_key, inst);
        LOG_INFO("BGP: Creating address family instance afi=%u safi=%u (VRF %u)", (unsigned)afi, (unsigned)safi,
                 vrf->vrf_id);
    }

    /* 若该邻居在此实例下已使能，直接返回 */
    if (g_hash_table_lookup(inst->peer_hash, addr))
    {
        char addr_str[64];
        net_addr_to_str(addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: Neighbor %s already enabled in instance afi=%u safi=%u", addr_str, (unsigned)afi,
                 (unsigned)safi);
        return 0;
    }

    /* 创建 per-AF peer，挂入 instance.peer_hash（inst 持有所有权） */
    bgp_peer_t *peer = bgp_peer_create(vrf, inst, addr);
    net_addr_t *peer_key = g_malloc(sizeof(net_addr_t));
    *peer_key = *addr;
    g_hash_table_insert(inst->peer_hash, peer_key, peer);

    /* 同时将借用引用加入 session->peer_list，便于通过 session 快速查询 */
    sess->peer_list = g_list_append(sess->peer_list, peer);

    char addr_str[64];
    net_addr_to_str(addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP: Neighbor %s enabled in instance afi=%u safi=%u (VRF %u)", addr_str, (unsigned)afi, (unsigned)safi,
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

    bgp_peer_t *peer = g_hash_table_lookup(inst->peer_hash, addr);
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
    g_hash_table_remove(inst->peer_hash, addr);

    char addr_str[64];
    net_addr_to_str(addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP: Neighbor %s disabled in instance afi=%u safi=%u (VRF %u)", addr_str, (unsigned)afi, (unsigned)safi,
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
        LOG_INFO("BGP: Creating address family instance afi=%u safi=%u (VRF %u)", (unsigned)afi, (unsigned)safi,
                 vrf->vrf_id);
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
    LOG_INFO("BGP: Deleting address family instance afi=%u safi=%u (VRF %u)", (unsigned)afi, (unsigned)safi,
             vrf->vrf_id);
}

void bgp_vrf_apply_update(bgp_vrf_t *vrf, const net_addr_t *src, const bgp_update_result_t *upd,
                          bgp_rib_update_stats_t *stats)
{
    if (stats)
    {
        memset(stats, 0, sizeof(*stats));
    }
    if (!vrf || !src || !upd)
    {
        return;
    }

    for (uint32_t i = 0; i < upd->reach_len; i++)
    {
        const bgp_nlri_entry_t *e = &upd->reach[i];
        bgp_instance_t *inst = bgp_vrf_get_or_create_instance(vrf, (bgp_afi_t)e->afi, (bgp_safi_t)e->safi);
        if (!inst || !inst->rib)
        {
            continue;
        }

        int rc = bgp_rib_reach_one(inst->rib, e, src, &upd->attr, &upd->nexthop);
        if (!stats)
        {
            continue;
        }
        if (rc == 1)
        {
            stats->reach_new++;
        }
        else if (rc == 0)
        {
            stats->reach_update++;
        }
    }

    for (uint32_t i = 0; i < upd->unreach_len; i++)
    {
        const bgp_nlri_entry_t *e = &upd->unreach[i];
        bgp_instance_t *inst =
            g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key((bgp_afi_t)e->afi, (bgp_safi_t)e->safi));
        if (!inst || !inst->rib)
        {
            if (stats)
            {
                stats->unreach_miss++;
            }
            continue;
        }

        int rc = bgp_rib_unreach_one(inst->rib, e, src);
        if (!stats)
        {
            continue;
        }
        if (rc == 1)
        {
            stats->unreach_removed++;
        }
        else if (rc == 0)
        {
            stats->unreach_miss++;
        }
    }
}

uint32_t bgp_vrf_purge_session_routes(bgp_vrf_t *vrf, const net_addr_t *addr)
{
    if (!vrf || !addr)
    {
        return 0;
    }

    uint32_t total_routes = 0;
    uint32_t total_heads = 0;

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, vrf->inst_hash);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        (void)key;
        bgp_instance_t *inst = (bgp_instance_t *)val;
        if (!inst || !inst->rib)
        {
            continue;
        }

        uint32_t removed_routes = 0;
        uint32_t removed_heads = 0;
        bgp_rib_remove_source(inst->rib, addr, &removed_routes, &removed_heads);
        total_routes += removed_routes;
        total_heads += removed_heads;
    }

    if (total_routes > 0)
    {
        char src_str[64];
        net_addr_to_str(addr, src_str, sizeof(src_str));
        LOG_INFO("BGP: VRF %u cleaning up neighbor %s routes: routes=%u heads=%u", vrf->vrf_id, src_str, total_routes,
                 total_heads);
    }

    return total_routes;
}

uint32_t bgp_vrf_rib_head_count(const bgp_vrf_t *vrf)
{
    if (!vrf)
    {
        return 0;
    }

    uint32_t total = 0;
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, (GHashTable *)vrf->inst_hash);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        (void)key;
        const bgp_instance_t *inst = (const bgp_instance_t *)val;
        total += bgp_rib_head_count(inst ? inst->rib : NULL);
    }
    return total;
}

uint32_t bgp_vrf_rib_route_count(const bgp_vrf_t *vrf)
{
    if (!vrf)
    {
        return 0;
    }

    uint32_t total = 0;
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, (GHashTable *)vrf->inst_hash);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        (void)key;
        const bgp_instance_t *inst = (const bgp_instance_t *)val;
        total += bgp_rib_route_count(inst ? inst->rib : NULL);
    }
    return total;
}
