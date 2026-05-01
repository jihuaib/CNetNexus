/**
 * @file   bgp_cfg_apply.c
 * @brief  BGP 配置内存态应用实现（CLI / DB 恢复共用）
 *
 * 每个函数负责：参数校验、同配置短路（设 NOOP）、内存更新、副作用，
 * 最终将结果写入 apply->rc 和 apply->errmsg。
 *
 * @author jhb
 * @date   2026/03/07
 */
#include "bgp_cfg_apply.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "bgp_attr_intern.h"
#include "bgp_calc.h"
#include "bgp_conn.h"
#include "bgp_if_cache.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_route_flush.h"
#include "bgp_session.h"
#include "bgp_update_group.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "route.h"

/* 在配置删除路径中，同步抽干 work 队列的最大轮次，避免销毁前遗留待撤销任务。 */
#define BGP_CFG_DRAIN_MAX_PASSES 1024u

static void bgp_cfg_drain_instance_work(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    for (uint32_t pass = 0; pass < BGP_CFG_DRAIN_MAX_PASSES; ++pass)
    {
        uint32_t calc = (inst->calc_queue) ? inst->calc_queue->count : 0u;
        uint32_t route_flush = (inst->route_flush_queue) ? inst->route_flush_queue->count : 0u;
        uint32_t pub = 0u;
        for (GList *ul = inst->update_groups; ul; ul = ul->next)
        {
            bgp_update_group_t *ug = (bgp_update_group_t *)ul->data;
            if (!ug)
            {
                continue;
            }
            for (GList *sl = ug->subgroups; sl; sl = sl->next)
            {
                bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
                if (!sg)
                {
                    continue;
                }
                if (sg->announce_queue)
                {
                    pub += (uint32_t)g_queue_get_length(sg->announce_queue);
                }
                if (sg->withdraw_queue)
                {
                    pub += (uint32_t)g_queue_get_length(sg->withdraw_queue);
                }
            }
        }
        if (calc == 0u && route_flush == 0u && pub == 0u)
        {
            return;
        }
        bgp_instance_drain_pending(inst);
    }
}

static void bgp_cfg_drain_vrf_work(bgp_vrf_t *vrf)
{
    if (!vrf || !vrf->inst_hash)
    {
        return;
    }

    GHashTableIter inst_iter;
    gpointer inst_key = NULL;
    gpointer inst_val = NULL;
    g_hash_table_iter_init(&inst_iter, vrf->inst_hash);
    while (g_hash_table_iter_next(&inst_iter, &inst_key, &inst_val))
    {
        (void)inst_key;
        bgp_cfg_drain_instance_work((bgp_instance_t *)inst_val);
    }
}

static void bgp_cfg_stop_all_sessions_and_drain_work(bgp_protocol_t *proto)
{
    if (!proto || !proto->vrf_hash)
    {
        return;
    }

    GHashTableIter vrf_iter;
    gpointer vrf_key = NULL;
    gpointer vrf_val = NULL;
    g_hash_table_iter_init(&vrf_iter, proto->vrf_hash);
    while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
    {
        (void)vrf_key;
        bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
        if (!vrf)
        {
            continue;
        }

        if (vrf->sess_hash)
        {
            GHashTableIter sess_iter;
            gpointer sess_key = NULL;
            gpointer sess_val = NULL;
            g_hash_table_iter_init(&sess_iter, vrf->sess_hash);
            while (g_hash_table_iter_next(&sess_iter, &sess_key, &sess_val))
            {
                (void)sess_key;
                bgp_session_stop_all((bgp_session_t *)sess_val);
            }
        }

        bgp_cfg_drain_vrf_work(vrf);
    }
}

/* ============================================================================
 * bgp / no bgp
 * ========================================================================== */

void bgp_cfg_apply_protocol(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (is_no)
    {
        if (!proto)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }
    }
    else
    {
        if (proto)
        {
            if (proto->as_number == apply->u.protocol.as_number)
            {
                apply->rc = BGP_APPLY_RC_NOOP;
                return;
            }
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: AS number mismatch.");
            return;
        }
        if (apply->u.protocol.as_number == 0)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Missing AS number.");
            return;
        }
    }

    if (is_no)
    {
        if (bgp_bmp_dispatch_clear_all() != 0)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to clear BMP instances.");
            return;
        }
        bgp_listen_stop();
        bgp_cfg_stop_all_sessions_and_drain_work(proto);
        bgp_protocol_destroy(g_bgp_work_local->protocol);
        g_bgp_work_local->protocol = NULL;
    }
    else
    {
        g_bgp_work_local->protocol = bgp_protocol_create(apply->u.protocol.as_number);
        if (!g_bgp_work_local->protocol)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply protocol configuration.");
            return;
        }
        bgp_listen_start();
    }
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * neighbor / no neighbor（BGP 视图）
 * ========================================================================== */

void bgp_cfg_apply_neighbor(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured. Run 'bgp <as-number>' first.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    bgp_session_t *existing = bgp_vrf_find_session(vrf, &apply->u.neighbor.addr);
    if (is_no)
    {
        if (!existing)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }
    }
    else
    {
        if (existing && existing->remote_as == apply->u.neighbor.remote_as)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }
        if (existing)
        {
            char addr_str[64];
            net_addr_to_str(&apply->u.neighbor.addr, addr_str, sizeof(addr_str));
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     "BGP Error: Neighbor %s remote-as cannot be modified in place. Delete the neighbor first.",
                     addr_str);
            apply->rc = BGP_APPLY_RC_FAIL;
            return;
        }
    }

    if (is_no)
    {
        /* 删除邻居时，级联删除该邻居在所有 AF 下的 peer 绑定 */
        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init(&iter, vrf->inst_hash);
        while (g_hash_table_iter_next(&iter, &key, &val))
        {
            (void)key;
            bgp_instance_t *inst = (bgp_instance_t *)val;
            if (!inst || !inst->peer_hash)
            {
                continue;
            }
            if (!g_hash_table_lookup(inst->peer_hash, &apply->u.neighbor.addr))
            {
                continue;
            }
            (void)bgp_vrf_af_disable_neighbor(vrf, inst->afi, inst->safi, &apply->u.neighbor.addr);
        }
        bgp_session_stop_all(existing);
        bgp_vrf_del_session(vrf, &apply->u.neighbor.addr);
        bgp_cfg_drain_vrf_work(vrf);
    }
    else
    {
        bgp_session_t *sess = bgp_session_create(&apply->u.neighbor.addr, apply->u.neighbor.remote_as, vrf);
        if (!sess)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply neighbor configuration.");
            return;
        }
        bgp_session_update_type(sess, proto->as_number);
        bgp_vrf_add_session(vrf, sess);
    }
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * address-family / no address-family
 * ========================================================================== */

void bgp_cfg_apply_instance(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    bgp_instance_t *inst =
        g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(apply->u.instance.afi, apply->u.instance.safi));
    if (is_no && !inst)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    if (!is_no && inst)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    if (is_no)
    {
        if (inst->peer_hash)
        {
            GList *addr_strs = NULL;
            GHashTableIter iter;
            gpointer k, v;
            g_hash_table_iter_init(&iter, inst->peer_hash);
            while (g_hash_table_iter_next(&iter, &k, &v))
            {
                addr_strs = g_list_append(addr_strs, g_strdup((const char *)k));
            }
            for (GList *l = addr_strs; l; l = l->next)
            {
                net_addr_t addr;
                if (net_addr_from_str((const char *)l->data, &addr) == 0)
                {
                    bgp_vrf_af_disable_neighbor(vrf, apply->u.instance.afi, apply->u.instance.safi, &addr);
                    bgp_session_t *sess = bgp_vrf_find_session(vrf, &addr);
                    if (sess && !bgp_vrf_neighbor_has_any_af(vrf, &addr))
                    {
                        bgp_session_stop_all(sess);
                    }
                }
            }
            g_list_free_full(addr_strs, g_free);
        }
        bgp_cfg_drain_instance_work(inst);
        bgp_vrf_del_instance(vrf, apply->u.instance.afi, apply->u.instance.safi);
    }
    else
    {
        inst = bgp_vrf_get_or_create_instance(vrf, apply->u.instance.afi, apply->u.instance.safi);
        if (!inst)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply instance configuration.");
            return;
        }
    }
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * AF 视图 neighbor enable / no neighbor
 * ========================================================================== */

void bgp_cfg_apply_af_neighbor(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.af_neighbor.addr);
    gpointer inst_key = bgp_inst_hash_key(apply->u.af_neighbor.afi, apply->u.af_neighbor.safi);

    /* 记录变更前状态（用于判断是否需要触发重协商） */
    bgp_instance_t *inst_before = g_hash_table_lookup(vrf->inst_hash, inst_key);
    gboolean had_af_before = (inst_before && inst_before->peer_hash &&
                              g_hash_table_lookup(inst_before->peer_hash, &apply->u.af_neighbor.addr));
    gboolean had_conn_before = (sess && (sess->pri_conn || sess->sec_conn));

    if (!is_no && !sess)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 "BGP Error: Neighbor session not configured. Run 'neighbor <ip> as <as>' first.");
        return;
    }

    if (is_no)
    {
        if (sess)
        {
            bgp_vrf_af_disable_neighbor(vrf, apply->u.af_neighbor.afi, apply->u.af_neighbor.safi,
                                        &apply->u.af_neighbor.addr);
            if (!bgp_vrf_neighbor_has_any_af(vrf, &apply->u.af_neighbor.addr))
            {
                bgp_session_stop_all(sess);
            }
        }
        bgp_cfg_drain_vrf_work(vrf);
    }
    else
    {
        gboolean first_af = !bgp_vrf_neighbor_has_any_af(vrf, &apply->u.af_neighbor.addr);
        if (bgp_vrf_af_enable_neighbor(vrf, apply->u.af_neighbor.afi, apply->u.af_neighbor.safi,
                                       &apply->u.af_neighbor.addr) != 0)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply AF neighbor configuration.");
            return;
        }
        if (first_af)
        {
            bgp_session_start_active(sess);
        }
    }

    /* 仅在 AF 成员关系实际变化时触发重协商，避免重复 enable 导致无意义 reset */
    bgp_instance_t *inst_after = g_hash_table_lookup(vrf->inst_hash, inst_key);
    gboolean has_af_after =
        (inst_after && inst_after->peer_hash && g_hash_table_lookup(inst_after->peer_hash, &apply->u.af_neighbor.addr));
    gboolean af_changed = (had_af_before != has_af_after);

    if (had_conn_before && af_changed && sess && (sess->pri_conn || sess->sec_conn))
    {
        bgp_neighbor_down(sess, g_bgp_work_local->epoll_fd);
    }
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * router-id / no router-id
 * ========================================================================== */

void bgp_cfg_apply_router_id(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    /* 同配置短路 */
    if (is_no && vrf->router_id == 0)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    if (!is_no && apply->u.router_id.id[0] != '\0')
    {
        struct in_addr cmp;
        if (inet_pton(AF_INET, apply->u.router_id.id, &cmp) == 1 && ntohl(cmp.s_addr) == vrf->router_id)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }
    }

    if (is_no)
    {
        vrf->router_id = 0;
    }
    else
    {
        struct in_addr addr;
        if (inet_pton(AF_INET, apply->u.router_id.id, &addr) != 1)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply router-id configuration.");
            return;
        }
        vrf->router_id = ntohl(addr.s_addr);
    }
    bgp_vrf_reset_all_sessions(vrf);
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * timers keepalive / no timers
 * ========================================================================== */

void bgp_cfg_apply_timers(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    /* 同配置短路 */
    if (is_no && vrf->keepalive == BGP_TIMER_DEFAULT_KEEPALIVE && vrf->hold_time == BGP_TIMER_DEFAULT_HOLD)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    if (!is_no && apply->u.timers.keepalive == vrf->keepalive && apply->u.timers.hold_time == vrf->hold_time)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    if (is_no)
    {
        vrf->keepalive = BGP_TIMER_DEFAULT_KEEPALIVE;
        vrf->hold_time = BGP_TIMER_DEFAULT_HOLD;
    }
    else
    {
        vrf->keepalive = apply->u.timers.keepalive;
        vrf->hold_time = apply->u.timers.hold_time;
    }
    bgp_vrf_reset_all_sessions(vrf);
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * timer connect-retry / no timer connect-retry
 * ========================================================================== */

void bgp_cfg_apply_connect_retry(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    /* 同配置短路 */
    if (is_no && vrf->connect_retry == BGP_TIMER_DEFAULT_CONNECT_RETRY)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    if (!is_no && apply->u.connect_retry.interval == vrf->connect_retry)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    if (is_no)
    {
        vrf->connect_retry = BGP_TIMER_DEFAULT_CONNECT_RETRY;
    }
    else
    {
        vrf->connect_retry = apply->u.connect_retry.interval;
    }
    bgp_vrf_rearm_retry_timers(vrf);
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * neighbor open-capability / no neighbor open-capability
 * ========================================================================== */

void bgp_cfg_apply_open_cap(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.open_cap.addr);
    if (!sess)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Neighbor not found.");
        return;
    }

    if (is_no)
    {
        BIT_CLR(sess->flags, apply->u.open_cap.cap_bit);
    }
    else
    {
        BIT_SET(sess->flags, apply->u.open_cap.cap_bit);
    }
    apply->out.sess_flags = sess->flags;
    bgp_neighbor_down(sess, g_bgp_work_local->epoll_fd);
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * import-route / no import-route
 * ========================================================================== */

void bgp_cfg_apply_import_route(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    bgp_instance_t *inst = bgp_vrf_get_or_create_instance(vrf, apply->u.import_route.afi, apply->u.import_route.safi);
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Address family instance creation failed.");
        return;
    }

    uint32_t proto_mask = 1U << apply->u.import_route.import_proto;
    if (is_no)
    {
        inst->import_protos &= ~proto_mask;
    }
    else
    {
        inst->import_protos |= proto_mask;
    }
    apply->out.import_protos = inst->import_protos;
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * neighbor source-interface / no neighbor source-interface
 * ========================================================================== */

static int bgp_cfg_resolve_source_if_addr(const char *if_name, sa_family_t peer_family, net_addr_t *out_addr,
                                          char *errmsg, size_t errmsg_len)
{
    return bgp_if_cache_resolve_source_addr(if_name, peer_family, out_addr, errmsg, errmsg_len);
}

void bgp_cfg_apply_source_if(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.source_if.addr);
    if (!sess)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 "BGP Error: Neighbor session not configured. Run 'neighbor <ip> as <as>' first.");
        return;
    }

    if (is_no)
    {
        if (sess->source_if_name[0] == '\0' && sess->source_addr.family == 0)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }

        sess->source_if_name[0] = '\0';
        memset(&sess->source_addr, 0, sizeof(sess->source_addr));

        if (sess->pri_conn || sess->sec_conn)
        {
            bgp_neighbor_down(sess, g_bgp_work_local->epoll_fd);
        }
        apply->rc = BGP_APPLY_RC_OK;
        return;
    }

    if (apply->u.source_if.if_name[0] == '\0')
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Missing source interface name.");
        return;
    }

    net_addr_t resolved_addr;
    memset(&resolved_addr, 0, sizeof(resolved_addr));
    if (bgp_cfg_resolve_source_if_addr(apply->u.source_if.if_name, sess->neighbor_addr.family, &resolved_addr,
                                       apply->errmsg, sizeof(apply->errmsg)) != 0)
    {
        return;
    }

    apply->u.source_if.source_addr = resolved_addr;
    if (strcmp(sess->source_if_name, apply->u.source_if.if_name) == 0 &&
        net_addr_cmp(&sess->source_addr, &resolved_addr) == 0)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    snprintf(sess->source_if_name, sizeof(sess->source_if_name), "%s", apply->u.source_if.if_name);
    sess->source_addr = resolved_addr;

    if (sess->pri_conn || sess->sec_conn)
    {
        bgp_neighbor_down(sess, g_bgp_work_local->epoll_fd);
    }
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * neighbor ebgp-multihop / no neighbor ebgp-multihop
 * ========================================================================== */

void bgp_cfg_apply_ebgp_multihop(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.ebgp_multihop.addr);
    if (!sess)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 "BGP Error: Neighbor session not configured. Run 'neighbor <ip> as <as>' first.");
        return;
    }

    if (is_no)
    {
        if (sess->ebgp_multihop_ttl == 0)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }

        sess->ebgp_multihop_ttl = 0;
        if (sess->pri_conn || sess->sec_conn)
        {
            bgp_neighbor_down(sess, g_bgp_work_local->epoll_fd);
        }
        apply->rc = BGP_APPLY_RC_OK;
        return;
    }

    if (apply->u.ebgp_multihop.ttl == 0)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Invalid ebgp-multihop TTL.");
        return;
    }

    if (sess->ebgp_multihop_ttl == apply->u.ebgp_multihop.ttl)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    sess->ebgp_multihop_ttl = apply->u.ebgp_multihop.ttl;
    if (sess->pri_conn || sess->sec_conn)
    {
        bgp_neighbor_down(sess, g_bgp_work_local->epoll_fd);
    }
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * QP 自产生路由 / route start-dqpn ...
 * ========================================================================== */

/**
 * @brief 计算 DQPN 编码所需字节数（1~3 字节）
 */
static uint8_t qp_dqpn_bytes(uint32_t dqpn)
{
    if (dqpn <= 0xFFU)
    {
        return 1u;
    }
    if (dqpn <= 0xFFFFU)
    {
        return 2u;
    }
    return 3u;
}

/**
 * @brief 构造一条 QP NLRI
 */
static void qp_build_nlri(bgp_nlri_entry_t *nlri, bgp_afi_t afi, uint32_t dqpn, const net_addr_t *ip, uint8_t mask_len)
{
    memset(nlri, 0, sizeof(*nlri));
    nlri->afi = (uint16_t)afi;
    nlri->safi = BGP_SAFI_QP;
    nlri->type = BGP_NLRI_QP;
    nlri->qp.dqpn = dqpn;
    nlri->qp.dqpn_len = qp_dqpn_bytes(dqpn);
    nlri->qp.prefix.prefix_len = mask_len;
    nlri->qp.prefix.addr = *ip;
}

/**
 * @brief 判断两个前缀是否相互覆盖（同族 + 较短前缀是较长前缀的祖先）
 */
static gboolean qp_prefix_overlap(const net_addr_t *a, uint8_t a_len, const net_addr_t *b, uint8_t b_len)
{
    if (a->family != b->family)
    {
        return FALSE;
    }
    uint8_t common = (a_len < b_len) ? a_len : b_len;
    const uint8_t *pa = NULL;
    const uint8_t *pb = NULL;
    if (a->family == AF_INET)
    {
        pa = (const uint8_t *)&a->u.v4.s_addr;
        pb = (const uint8_t *)&b->u.v4.s_addr;
    }
    else
    {
        pa = a->u.v6.s6_addr;
        pb = b->u.v6.s6_addr;
    }
    uint8_t full = common / 8u;
    uint8_t rem = common % 8u;
    for (uint8_t i = 0; i < full; i++)
    {
        if (pa[i] != pb[i])
        {
            return FALSE;
        }
    }
    if (rem > 0)
    {
        uint8_t mask = (uint8_t)(0xFFu << (8u - rem));
        if ((pa[full] & mask) != (pb[full] & mask))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean qp_route_cfg_equal(const bgp_qp_route_cfg_t *a, const bgp_qp_route_cfg_t *b)
{
    if (a->start_dqpn != b->start_dqpn || a->count != b->count || a->mask_len != b->mask_len)
    {
        return FALSE;
    }
    if (net_addr_cmp(&a->ip, &b->ip) != 0 || net_addr_cmp(&a->bid, &b->bid) != 0)
    {
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief 注入（或撤销）一条 qp_routes 配置覆盖的所有 NLRI
 */
static int qp_inject_cfg_entries(bgp_instance_t *inst, const bgp_qp_route_cfg_t *cfg, gboolean withdraw)
{
    if (!inst || !cfg)
    {
        return -1;
    }
    /* QP 是非 VPN AF，公网 RIB 即唯一 RIB */
    bgp_rib_t *rib = bgp_inst_public_rib(inst);
    if (!rib)
    {
        return -1;
    }

    bgp_nexthop_t nexthop;
    memset(&nexthop, 0, sizeof(nexthop));
    nexthop.global = cfg->bid;
    nexthop.has_link_local = false;

    bgp_attr_t attr;
    bgp_attr_build_imported(&attr);

    /* source 使用 BID 作为区分键（每条配置用相同 BID，故同 cfg 内所有 NLRI 共享 source） */
    net_addr_t src = cfg->bid;

    for (uint32_t i = 0; i < cfg->count; i++)
    {
        uint32_t dqpn = cfg->start_dqpn + i;
        bgp_nlri_entry_t nlri;
        qp_build_nlri(&nlri, inst->afi, dqpn, &cfg->ip, cfg->mask_len);

        if (withdraw)
        {
            int rc = bgp_rib_unreach_one(rib, &nlri, &src);
            if (rc == 1 && inst->calc_queue)
            {
                bgp_calc_queue_push(inst->calc_queue, inst, &nlri);
            }
            continue;
        }

        bgp_rthead_t *head = bgp_rib_ensure_head(rib, &nlri);
        if (!head)
        {
            continue;
        }
        bgp_route_node_t *route = bgp_rthead_lookup_route_mut(head, &src);
        if (!route)
        {
            route = bgp_rthead_create_route(rib, head, &src);
            if (!route)
            {
                continue;
            }
        }
        if (bgp_rib_route_apply_reach(route, ROUTE_PROTOCOL_STATIC, &attr, &nexthop) != 0)
        {
            continue;
        }
        BIT_SET(route->flags, BGP_ROUTE_FLAG_IMPORT);
        if (inst->calc_queue)
        {
            bgp_calc_queue_push(inst->calc_queue, inst, &nlri);
        }
    }
    return 0;
}

void bgp_cfg_apply_qp_route(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }
    if (apply->u.qp_route.safi != BGP_SAFI_QP)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: QP address family required.");
        return;
    }
    bgp_instance_t *inst =
        (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(apply->u.qp_route.afi, BGP_SAFI_QP));
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: QP address family not enabled.");
        return;
    }

    /* 参数校验 */
    uint8_t max_mask = (apply->u.qp_route.afi == BGP_AFI_IPV4) ? 32u : 128u;
    if (apply->u.qp_route.mask_len == 0 || apply->u.qp_route.mask_len > max_mask)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Invalid mask length.");
        return;
    }
    if (apply->u.qp_route.count == 0 ||
        (uint64_t)apply->u.qp_route.start_dqpn + apply->u.qp_route.count - 1u > 0xFFFFFFU)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: DQPN range out of bounds.");
        return;
    }
    sa_family_t expected = (apply->u.qp_route.afi == BGP_AFI_IPV4) ? AF_INET : AF_INET6;
    if (apply->u.qp_route.ip.family != expected)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Prefix family mismatch.");
        return;
    }
    if (apply->u.qp_route.bid.family != AF_INET6)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BID must be IPv6.");
        return;
    }

    bgp_qp_route_cfg_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.start_dqpn = apply->u.qp_route.start_dqpn;
    tmp.count = apply->u.qp_route.count;
    tmp.ip = apply->u.qp_route.ip;
    tmp.mask_len = apply->u.qp_route.mask_len;
    tmp.bid = apply->u.qp_route.bid;

    if (is_no)
    {
        GList *node = NULL;
        for (GList *l = inst->qp_routes; l; l = l->next)
        {
            bgp_qp_route_cfg_t *cfg = (bgp_qp_route_cfg_t *)l->data;
            if (cfg && qp_route_cfg_equal(cfg, &tmp))
            {
                node = l;
                break;
            }
        }
        if (!node)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }
        bgp_qp_route_cfg_t *cfg = (bgp_qp_route_cfg_t *)node->data;
        (void)qp_inject_cfg_entries(inst, cfg, TRUE);
        inst->qp_routes = g_list_delete_link(inst->qp_routes, node);
        g_free(cfg);
        apply->rc = BGP_APPLY_RC_OK;
        return;
    }

    /* 新增：前缀覆盖检查 */
    for (GList *l = inst->qp_routes; l; l = l->next)
    {
        const bgp_qp_route_cfg_t *cfg = (const bgp_qp_route_cfg_t *)l->data;
        if (!cfg)
        {
            continue;
        }
        if (qp_route_cfg_equal(cfg, &tmp))
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }
        if (qp_prefix_overlap(&cfg->ip, cfg->mask_len, &tmp.ip, tmp.mask_len))
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Prefix overlaps existing QP route.");
            return;
        }
    }

    bgp_qp_route_cfg_t *stored = g_malloc(sizeof(*stored));
    *stored = tmp;
    inst->qp_routes = g_list_append(inst->qp_routes, stored);
    (void)qp_inject_cfg_entries(inst, stored, FALSE);
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * route-select enable / no route-select enable
 * ========================================================================== */

typedef struct
{
    bgp_instance_t *inst;
} qp_rehash_ctx_t;

static gboolean qp_rehash_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    qp_rehash_ctx_t *ctx = (qp_rehash_ctx_t *)data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;
    if (ctx && ctx->inst && ctx->inst->calc_queue && head)
    {
        bgp_calc_queue_push(ctx->inst->calc_queue, ctx->inst, &head->nlri);
    }
    return FALSE;
}

void bgp_cfg_apply_route_select(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }
    if (apply->u.route_select.safi != BGP_SAFI_QP)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: QP address family required.");
        return;
    }
    bgp_instance_t *inst = (bgp_instance_t *)g_hash_table_lookup(
        vrf->inst_hash, bgp_inst_hash_key(apply->u.route_select.afi, BGP_SAFI_QP));
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: QP address family not enabled.");
        return;
    }

    bool want = !is_no;
    if (inst->route_select_enabled == want)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    inst->route_select_enabled = want;

    /* 重新优选所有 rthead：触发 announce（开启时）或 withdraw（关闭时）。QP 仅公网 RIB。 */
    {
        bgp_rib_t *rib = bgp_inst_public_rib(inst);
        if (rib && rib->head_tree)
        {
            qp_rehash_ctx_t ctx = {inst};
            g_tree_foreach(rib->head_tree, qp_rehash_cb, &ctx);
        }
    }
    apply->rc = BGP_APPLY_RC_OK;
}
