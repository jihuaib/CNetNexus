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

#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_protocol.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "db.h"
#include "errcode.h"

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
        if (inst->peer_hash)
        {
            GHashTableIter iter;
            gpointer key = NULL;
            gpointer val = NULL;
            g_hash_table_iter_init(&iter, inst->peer_hash);
            while (g_hash_table_iter_next(&iter, &key, &val))
            {
                (void)val;
                bgp_session_t *sess = bgp_vrf_find_session(inst->vrf, (const net_addr_t *)key);
                if (sess && sess->pub_queue)
                {
                    pub += bgp_pub_queue_count_for_instance(sess->pub_queue, inst);
                }
            }
        }
        if (calc == 0u && route_flush == 0u && pub == 0u)
        {
            return;
        }
        bgp_work_process_pending(inst);
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
                bgp_server_stop_session_conns((bgp_session_t *)sess_val);
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
        bgp_server_stop_session_conns(existing);
        bgp_vrf_del_session(vrf, &apply->u.neighbor.addr);
        bgp_cfg_drain_vrf_work(vrf);
    }
    else if (existing)
    {
        existing->remote_as = apply->u.neighbor.remote_as;
    }
    else
    {
        bgp_session_t *sess = bgp_session_create(&apply->u.neighbor.addr, apply->u.neighbor.remote_as, vrf);
        if (!sess)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply neighbor configuration.");
            return;
        }
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
                        bgp_server_stop_session_conns(sess);
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
                bgp_server_stop_session_conns(sess);
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
            bgp_server_start_active_conn(sess);
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
    bgp_server_reset_all_sessions(vrf);
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
    bgp_server_reset_all_sessions(vrf);
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
    bgp_server_rearm_retry_timers(vrf);
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
    if (!if_name || if_name[0] == '\0' || !out_addr)
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Missing source interface name.");
        }
        return -1;
    }

    db_condition_t cond = {
        .field_name = "name",
        .op = DB_CMP_EQ,
        .value = db_value_text(if_name),
    };
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};

    db_result_t *result = NULL;
    int qret = db_rpc_query(g_bgp_local->dev_ipc_ctx, "if_interface", NULL, 0, &filter, &result);
    db_value_free(&cond.value);

    if (qret != ERRCODE_SUCCESS || !result)
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Failed to query interface '%s'.", if_name);
        }
        db_result_free(result);
        return -1;
    }

    if (result->num_rows == 0)
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Interface '%s' not found.", if_name);
        }
        db_result_free(result);
        return -1;
    }

    const db_row_t *row = result->rows[0];
    const char *ip_str = NULL;
    if (peer_family == AF_INET6)
    {
        ip_str = db_row_get_text(row, "ipv6_address", NULL);
    }
    else
    {
        ip_str = db_row_get_text(row, "ip_address", NULL);
    }

    if ((!ip_str || ip_str[0] == '\0') && peer_family == 0)
    {
        /* 未指定对端地址族时，按 IPv4 -> IPv6 回退。 */
        ip_str = db_row_get_text(row, "ip_address", NULL);
        if (!ip_str || ip_str[0] == '\0')
        {
            ip_str = db_row_get_text(row, "ipv6_address", NULL);
        }
    }

    if (!ip_str || ip_str[0] == '\0')
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Interface '%s' has no usable IP address.", if_name);
        }
        db_result_free(result);
        return -1;
    }

    net_addr_t addr;
    if (net_addr_from_str(ip_str, &addr) != 0)
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Interface '%s' has invalid IP '%s'.", if_name, ip_str);
        }
        db_result_free(result);
        return -1;
    }

    if (peer_family != 0 && addr.family != peer_family)
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Interface '%s' address family mismatch with neighbor.", if_name);
        }
        db_result_free(result);
        return -1;
    }

    *out_addr = addr;
    db_result_free(result);
    return 0;
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
