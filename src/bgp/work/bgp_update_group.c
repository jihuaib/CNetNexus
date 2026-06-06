/**
 * @file   bgp_update_group.c
 * @brief  BGP Update Group / NH Subgroup 生命周期与 peer 归组
 * @author jhb
 * @date   2026/04/10
 */
#include "bgp_update_group.h"

#include <string.h>
#include <sys/socket.h>

#include "bgp_adj_rib_out.h"
#include "bgp_attr_intern.h"
#include "bgp_conn.h"
#include "bgp_fsm.h"
#include "bgp_instance.h"
#include "bgp_nexthop.h"
#include "bgp_peer.h"
#include "bgp_pkt.h"
#include "bgp_rd.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"
#include "bgp_vrf_export.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

/** 自增 group_id，仅用于日志/调试 */
static uint32_t g_next_group_id = 1;

/* ============================================================================
 * 键相等判断
 * ========================================================================== */

static gboolean ug_key_equal(const bgp_update_group_key_t *a, const bgp_update_group_key_t *b)
{
    return a->sess_type == b->sess_type && a->policy_hash == b->policy_hash && a->peer_family == b->peer_family &&
           a->remote_as == b->remote_as && a->negotiated_caps == b->negotiated_caps && a->flags == b->flags;
}

static gboolean sg_key_equal(const bgp_nh_subgroup_key_t *a, const bgp_nh_subgroup_key_t *b)
{
    if (a->rule != b->rule)
    {
        return FALSE;
    }
    /* 非 R_LOCAL 的子组 key 不含地址维度（effective_local_addr 约定为全零，
     * 但 net_addr_equal 对 family=0 会返回 FALSE，这里直接判相等） */
    if (a->rule != BGP_NH_RULE_LOCAL)
    {
        return TRUE;
    }
    return net_addr_equal(&a->effective_local_addr, &b->effective_local_addr);
}

/* ============================================================================
 * 键计算
 * ========================================================================== */

void bgp_session_compute_ug_key(const bgp_session_t *sess, bgp_update_group_key_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!sess)
    {
        out->sess_type = BGP_SESS_TYPE_UNKNOWN;
        return;
    }
    out->sess_type = sess->sess_type;
    out->policy_hash = 0; /* 预留：Phase 4 引入 route-map 时填入 */
    out->peer_family = (uint16_t)sess->neighbor_addr.family;
    out->remote_as = sess->remote_as;
    out->negotiated_caps = sess->negotiated_caps;
    out->flags = 0;
}

void bgp_peer_compute_ug_key(const bgp_peer_t *peer, const bgp_session_t *sess, bgp_update_group_key_t *out)
{
    bgp_session_compute_ug_key(sess, out);
    if (peer && BIT_TEST(peer->flags, BGP_PEER_FLAG_RR_CLIENT))
    {
        BIT_SET(out->flags, BGP_UG_FLAG_TARGET_RR_CLIENT);
    }
}

void bgp_session_compute_sg_key(const bgp_session_t *sess, bgp_nh_subgroup_key_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!sess)
    {
        return;
    }
    /* 旧接口的默认行为：回退到 R_LOCAL + effective_local_addr，方便老代码兼容。 */
    out->rule = BGP_NH_RULE_LOCAL;
    if (sess->pri_conn && sess->pri_conn->fd >= 0)
    {
        net_addr_t local = {0};
        if (bgp_conn_get_local_addr(sess->pri_conn, &local) == 0)
        {
            out->effective_local_addr = local;
        }
    }
}

/* ============================================================================
 * Subgroup 生命周期
 * ========================================================================== */

static bgp_nh_subgroup_t *nh_subgroup_new(const bgp_nh_subgroup_key_t *key, bgp_update_group_t *parent)
{
    bgp_nh_subgroup_t *sg = g_malloc0(sizeof(bgp_nh_subgroup_t));
    sg->key = *key;
    sg->parent = parent;
    sg->peer_list = NULL;
    sg->peer_count = 0;
    sg->adj_rib_out = bgp_adj_rib_out_create();
    sg->announce_queue = g_queue_new();
    sg->withdraw_queue = g_queue_new();
    sg->announce_count = 0;
    sg->withdraw_count = 0;
    return sg;
}

void bgp_nh_subgroup_destroy(bgp_nh_subgroup_t *sg)
{
    if (!sg)
    {
        return;
    }
    /* peer_list 为借用引用，不持有所有权；只释放链表节点本身 */
    g_list_free(sg->peer_list);
    sg->peer_list = NULL;

    if (sg->announce_queue)
    {
        g_queue_free_full(sg->announce_queue, g_free);
        sg->announce_queue = NULL;
    }
    if (sg->withdraw_queue)
    {
        g_queue_free_full(sg->withdraw_queue, g_free);
        sg->withdraw_queue = NULL;
    }

    if (sg->adj_rib_out)
    {
        bgp_adj_rib_out_destroy(sg->adj_rib_out);
        sg->adj_rib_out = NULL;
    }

    g_free(sg);
}

bgp_nh_subgroup_t *bgp_nh_subgroup_find_or_create(bgp_update_group_t *ug, const bgp_nh_subgroup_key_t *key)
{
    if (!ug || !key)
    {
        return NULL;
    }
    for (GList *l = ug->subgroups; l; l = l->next)
    {
        bgp_nh_subgroup_t *sg = l->data;
        if (sg && sg_key_equal(&sg->key, key))
        {
            return sg;
        }
    }
    bgp_nh_subgroup_t *sg = nh_subgroup_new(key, ug);
    ug->subgroups = g_list_append(ug->subgroups, sg);
    ug->subgroup_count++;
    return sg;
}

bgp_nh_subgroup_t *bgp_update_group_find_subgroup_by_rule(const bgp_update_group_t *ug, bgp_nh_rule_t rule)
{
    if (!ug)
    {
        return NULL;
    }
    for (GList *l = ug->subgroups; l; l = l->next)
    {
        bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)l->data;
        if (sg && sg->key.rule == rule)
        {
            return sg;
        }
    }
    return NULL;
}

void bgp_nh_subgroup_remove_if_empty(bgp_update_group_t *ug, bgp_nh_subgroup_t *sg)
{
    if (!ug || !sg)
    {
        return;
    }
    if (sg->peer_count > 0)
    {
        return;
    }
    ug->subgroups = g_list_remove(ug->subgroups, sg);
    if (ug->subgroup_count > 0)
    {
        ug->subgroup_count--;
    }
    bgp_nh_subgroup_destroy(sg);
}

/* ============================================================================
 * Update Group 生命周期
 * ========================================================================== */

static bgp_update_group_t *update_group_new(const bgp_update_group_key_t *key, bgp_instance_t *inst)
{
    bgp_update_group_t *ug = g_malloc0(sizeof(bgp_update_group_t));
    ug->key = *key;
    ug->inst = inst;
    ug->subgroups = NULL;
    ug->subgroup_count = 0;
    ug->group_id = g_next_group_id++;
    return ug;
}

void bgp_update_group_destroy(bgp_update_group_t *ug)
{
    if (!ug)
    {
        return;
    }
    for (GList *l = ug->subgroups; l; l = l->next)
    {
        bgp_nh_subgroup_destroy((bgp_nh_subgroup_t *)l->data);
    }
    g_list_free(ug->subgroups);
    ug->subgroups = NULL;
    g_free(ug);
}

bgp_update_group_t *bgp_update_group_find_or_create(bgp_instance_t *inst, const bgp_update_group_key_t *key)
{
    if (!inst || !key)
    {
        return NULL;
    }
    for (GList *l = inst->update_groups; l; l = l->next)
    {
        bgp_update_group_t *ug = l->data;
        if (ug && ug_key_equal(&ug->key, key))
        {
            return ug;
        }
    }
    bgp_update_group_t *ug = update_group_new(key, inst);
    inst->update_groups = g_list_append(inst->update_groups, ug);
    LOG_DEBUG("BGP: update_group created id=%u sess_type=%d peer_family=%u afi=%u safi=%u", ug->group_id,
              (int)key->sess_type, (unsigned)key->peer_family, (unsigned)inst->afi, (unsigned)inst->safi);
    return ug;
}

void bgp_update_group_remove_if_empty(bgp_instance_t *inst, bgp_update_group_t *ug)
{
    if (!inst || !ug)
    {
        return;
    }
    if (ug->subgroup_count > 0)
    {
        return;
    }
    inst->update_groups = g_list_remove(inst->update_groups, ug);
    LOG_DEBUG("BGP: update_group destroyed id=%u afi=%u safi=%u", ug->group_id, (unsigned)inst->afi,
              (unsigned)inst->safi);
    bgp_update_group_destroy(ug);
}

/* ============================================================================
 * Rule 分派（per-route）
 * ========================================================================== */

bgp_route_src_class_t bgp_classify_route_src(const bgp_route_node_t *best)
{
    if (!best)
    {
        return BGP_RSRC_IMPORT;
    }
    /* 本地起源（重分发 IMPORT 或 vrf-export 本地跨表 LOCAL_CROSS）：按本地源处理 */
    if (bgp_route_is_local_origin(best))
    {
        return BGP_RSRC_IMPORT;
    }
    /* 非本地起源路由：根据源 session 类型分类 */
    if (best->head && best->head->inst && best->head->inst->vrf)
    {
        const bgp_session_t *src = bgp_vrf_find_session(best->head->inst->vrf, (net_addr_t *)&best->source);
        if (src)
        {
            return (src->sess_type == BGP_SESS_TYPE_EBGP) ? BGP_RSRC_FROM_EBGP : BGP_RSRC_FROM_IBGP;
        }
    }
    /* 无法定位来源：当作 FROM_EBGP（保守使用 PASS 语义由调用方处理） */
    return BGP_RSRC_FROM_EBGP;
}

bool bgp_select_nh_rule(const bgp_update_group_key_t *ug_key, bgp_route_src_class_t src_class, bgp_nh_rule_t *out_rule)
{
    if (!ug_key || !out_rule)
    {
        return false;
    }
    if (ug_key->sess_type == BGP_SESS_TYPE_EBGP)
    {
        /* 向 eBGP 发布：一律 LOCAL（next-hop-self） */
        *out_rule = BGP_NH_RULE_LOCAL;
        return true;
    }
    /* 向 iBGP 发布 */
    switch (src_class)
    {
        case BGP_RSRC_IMPORT:
            *out_rule = BGP_NH_RULE_LOCAL; /* 本地 IMPORT：用本端地址 */
            return true;
        case BGP_RSRC_FROM_EBGP:
            *out_rule = BGP_NH_RULE_PASS; /* 保留 eBGP 原始 nh */
            return true;
        case BGP_RSRC_FROM_IBGP:
        default:
            /* RR 场景：目标 peer 是客户端时，反射 iBGP 路由（保留原 nh） */
            if (BIT_TEST(ug_key->flags, BGP_UG_FLAG_TARGET_RR_CLIENT))
            {
                *out_rule = BGP_NH_RULE_PASS;
                return true;
            }
            /* 非 RR 场景：iBGP split-horizon，不转发 */
            return false;
    }
}

/* ============================================================================
 * Peer 加入/离开
 * ========================================================================== */

/** 计算 peer 在本 ug 下应该加入哪些 rule 的子组 */
static int compute_rules_for_ug(const bgp_update_group_key_t *ug_key, bgp_nh_rule_t *rules_out, int cap)
{
    int n = 0;
    if (cap < 1)
    {
        return 0;
    }
    if (ug_key->sess_type == BGP_SESS_TYPE_EBGP)
    {
        rules_out[n++] = BGP_NH_RULE_LOCAL;
    }
    else
    {
        /* iBGP 需要同时具备 LOCAL（IMPORT 路由用）和 PASS（eBGP-learned 转发用） */
        rules_out[n++] = BGP_NH_RULE_LOCAL;
        if (n < cap)
        {
            rules_out[n++] = BGP_NH_RULE_PASS;
        }
    }
    return n;
}

void bgp_subgroup_peer_join(bgp_peer_t *peer, bgp_session_t *sess)
{
    if (!peer || !peer->inst || !sess)
    {
        return;
    }
    bgp_instance_t *inst = peer->inst;

    /* 幂等：已在某些 subgroup 中，先全部 leave */
    if (peer->subgroups)
    {
        bgp_subgroup_peer_leave(peer, sess);
    }

    bgp_update_group_key_t ugk;
    bgp_peer_compute_ug_key(peer, sess, &ugk);
    /* NH_UNCHANGED 仅影响 nexthop 规则，不改变 update-group 划分。 */
    bgp_update_group_t *ug = bgp_update_group_find_or_create(inst, &ugk);
    if (!ug)
    {
        return;
    }

    /* 缓存本端连接地址（R_LOCAL 子组键需要） */
    net_addr_t local = {0};
    if (sess->pri_conn && sess->pri_conn->fd >= 0)
    {
        (void)bgp_conn_get_local_addr(sess->pri_conn, &local);
    }

    bgp_nh_rule_t rules[4];
    int nrules;
    if (inst->flags & BGP_INST_FLAG_NH_UNCHANGED)
    {
        /* QP/next-hop-unchanged：沿用正常 UG 划分，但所有导出统一保留原 nexthop。 */
        nrules = 1;
        rules[0] = BGP_NH_RULE_PASS;
    }
    else
    {
        nrules = compute_rules_for_ug(&ugk, rules, (int)(sizeof(rules) / sizeof(rules[0])));
    }

    for (int i = 0; i < nrules; i++)
    {
        bgp_nh_subgroup_key_t sgk;
        memset(&sgk, 0, sizeof(sgk));
        sgk.rule = rules[i];
        if (rules[i] == BGP_NH_RULE_LOCAL)
        {
            sgk.effective_local_addr = local;
        }

        bgp_nh_subgroup_t *sg = bgp_nh_subgroup_find_or_create(ug, &sgk);
        if (!sg)
        {
            continue;
        }
        /* 避免重复加入同一子组 */
        if (g_list_find(sg->peer_list, peer))
        {
            continue;
        }
        sg->peer_list = g_list_append(sg->peer_list, peer);
        sg->peer_count++;
        peer->subgroups = g_list_append(peer->subgroups, sg);
    }

    char addr_buf[64];
    net_addr_to_str(&peer->addr, addr_buf, sizeof(addr_buf));
    LOG_DEBUG("BGP: peer %s joined %d subgroup(s) in ug_id=%u afi=%u safi=%u", addr_buf, nrules, ug->group_id,
              (unsigned)inst->afi, (unsigned)inst->safi);
}

void bgp_subgroup_peer_leave(bgp_peer_t *peer, bgp_session_t *sess)
{
    if (!peer || !peer->subgroups)
    {
        return;
    }
    bgp_instance_t *inst = peer->inst;

    /* 先收集待清理的 ug，避免在迭代中销毁 */
    GList *ug_to_check = NULL;

    for (GList *l = peer->subgroups; l; l = l->next)
    {
        bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)l->data;
        if (!sg)
        {
            continue;
        }
        sg->peer_list = g_list_remove(sg->peer_list, peer);
        if (sg->peer_count > 0)
        {
            sg->peer_count--;
        }
        bgp_update_group_t *ug = sg->parent;
        if (sg->peer_count == 0)
        {
            bgp_nh_subgroup_remove_if_empty(ug, sg);
            if (ug && !g_list_find(ug_to_check, ug))
            {
                ug_to_check = g_list_prepend(ug_to_check, ug);
            }
        }
    }

    g_list_free(peer->subgroups);
    peer->subgroups = NULL;

    for (GList *l = ug_to_check; l; l = l->next)
    {
        bgp_update_group_t *ug = (bgp_update_group_t *)l->data;
        if (ug && ug->subgroup_count == 0)
        {
            bgp_update_group_remove_if_empty(inst, ug);
        }
    }
    g_list_free(ug_to_check);

    char addr_buf[64];
    if (sess)
    {
        net_addr_to_str(&sess->neighbor_addr, addr_buf, sizeof(addr_buf));
    }
    else
    {
        net_addr_to_str(&peer->addr, addr_buf, sizeof(addr_buf));
    }
    LOG_DEBUG("BGP: peer %s left all subgroups afi=%u safi=%u", addr_buf, inst ? (unsigned)inst->afi : 0u,
              inst ? (unsigned)inst->safi : 0u);
}

/* ============================================================================
 * 遍历
 * ========================================================================== */

void bgp_instance_foreach_subgroup(bgp_instance_t *inst, bgp_subgroup_cb cb, gpointer user_data)
{
    if (!inst || !cb)
    {
        return;
    }
    for (GList *ul = inst->update_groups; ul; ul = ul->next)
    {
        bgp_update_group_t *ug = ul->data;
        if (!ug)
        {
            continue;
        }
        for (GList *sl = ug->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = sl->data;
            if (sg)
            {
                cb(sg, user_data);
            }
        }
    }
}

// ============================================================================
// 子组级发布通路：属性评估 / 入队 / 补发 / 调度 / 打包发送
// ============================================================================

static void bgp_update_group_schedule_pub(bgp_instance_t *inst);
static int bgp_update_group_process_pub_event(bgp_instance_t *inst, gboolean allow_reschedule);

static gboolean bgp_session_is_publish_ready(const bgp_session_t *sess)
{
    return sess && sess->pri_conn && sess->pri_conn->fd >= 0 && sess->fsm_state == BGP_FSM_STATE_ESTABLISHED;
}

static uint32_t bgp_update_group_local_as(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->protocol)
    {
        return 0u;
    }
    return g_bgp_work_local->protocol->as_number;
}

static const bgp_session_t *bgp_best_source_session(const bgp_route_node_t *best)
{
    if (!best || bgp_route_is_local_origin(best) || !best->head || !best->head->inst || !best->head->inst->vrf)
    {
        return NULL;
    }
    return bgp_vrf_find_session(best->head->inst->vrf, &best->source);
}

/**
 * @brief 按子组 rule + effective_local_addr 计算 out_nh
 *
 * R_LOCAL：使用子组缓存的本端连接地址替换 nexthop（eBGP 的 next-hop-self、iBGP
 *          对 IMPORT 路由的反射均走此路径）；处理双栈/RFC 8950 family 转换。
 * R_PASS： 保留 best 的 nhobj key.nexthop 原值。
 * R_CONFIG：预留未实现，当前回退到 PASS。
 */
static bool bgp_subgroup_apply_nexthop(const bgp_nh_subgroup_t *sg, const bgp_route_node_t *best, bgp_nexthop_t *out_nh)
{
    if (bgp_nexthop_get_route_bgp(best, out_nh) != ERRCODE_SUCCESS)
    {
        return false;
    }

    if (!sg || sg->key.rule != BGP_NH_RULE_LOCAL)
    {
        return true; /* PASS / CONFIG：保留原 nh */
    }

    const net_addr_t *local = &sg->key.effective_local_addr;
    if (local->family == 0 || net_addr_is_zero(local))
    {
        return true; /* 本端地址无效：无法替换，保留原 nh */
    }

    /* 同族 nexthop 替换（传统场景） */
    if (local->family == out_nh->global.family || out_nh->global.family == 0)
    {
        out_nh->global = *local;
        out_nh->has_link_local = false;
        memset(&out_nh->link_local, 0, sizeof(out_nh->link_local));
        return true;
    }

    /* 双栈：IPv6 路由 + IPv4 本端 → 使用本端 IPv4 */
    if (local->family == AF_INET && out_nh->global.family == AF_INET6)
    {
        out_nh->global = *local;
        out_nh->has_link_local = false;
        memset(&out_nh->link_local, 0, sizeof(out_nh->link_local));
        return true;
    }

    /* RFC 8950：IPv4 路由 + IPv6 本端（需 EXT_NEXTHOP 能力）
     * 子组级无法精确判断每个 session 的能力；保守做法——只要本端是 IPv6 就替换，
     * 具体 session 若未协商 EXT_NEXTHOP 将由发送阶段检查过滤。 */
    if (local->family == AF_INET6 && out_nh->global.family == AF_INET)
    {
        out_nh->global = *local;
        out_nh->has_link_local = false;
        memset(&out_nh->link_local, 0, sizeof(out_nh->link_local));
    }
    return true;
}

bool bgp_subgroup_eval_export(const bgp_nh_subgroup_t *sg, const bgp_route_node_t *best, const bgp_nlri_entry_t *nlri,
                              bgp_attr_t *out_attr, bgp_nexthop_t *out_nh)
{
    (void)nlri;
    if (!sg || !best || !out_attr || !out_nh)
    {
        return false;
    }

    bgp_sess_type_t stype = sg->parent ? sg->parent->key.sess_type : BGP_SESS_TYPE_UNKNOWN;
    bool target_is_client = sg->parent ? BIT_TEST(sg->parent->key.flags, BGP_UG_FLAG_TARGET_RR_CLIENT) : false;
    bool is_reflecting = false;

    /* iBGP→iBGP 反射检查：仅当目标 peer 是 RR client 时允许反射，否则 split-horizon */
    if (stype == BGP_SESS_TYPE_IBGP && !bgp_route_is_local_origin(best))
    {
        const bgp_session_t *src_sess = bgp_best_source_session(best);
        if (src_sess && src_sess->sess_type == BGP_SESS_TYPE_IBGP)
        {
            if (!target_is_client)
            {
                return false;
            }
            is_reflecting = true;
        }
    }

    /* 属性准备：复制 + eBGP AS_PATH prepend */
    memcpy(out_attr, BGP_ROUTE_ATTR(best), sizeof(*out_attr));

    /* RR 反射：注入 ORIGINATOR_ID + 在 CLUSTER_LIST 头部 prepend 本端 cluster-id（RFC 4456 §8） */
    if (is_reflecting)
    {
        const bgp_session_t *src_sess = bgp_best_source_session(best);
        if (!out_attr->has_originator_id && src_sess)
        {
            /* ORIGINATOR_ID = 路由原始 iBGP peer 的 BGP Identifier */
            memset(&out_attr->originator_id, 0, sizeof(out_attr->originator_id));
            out_attr->originator_id.family = AF_INET;
            out_attr->originator_id.u.v4.s_addr = htonl(src_sess->remote_id);
            out_attr->has_originator_id = true;
        }
        /* 出向 cluster-id 取自目标 AF 实例 */
        const bgp_instance_t *inst = (sg && sg->parent) ? sg->parent->inst : NULL;
        uint32_t cid = bgp_inst_effective_cluster_id(inst);
        if (cid != 0 &&
            out_attr->cluster_list_len < (uint8_t)(sizeof(out_attr->cluster_list) / sizeof(out_attr->cluster_list[0])))
        {
            /* 在头部 prepend 本端 cluster-id */
            for (int i = out_attr->cluster_list_len; i > 0; i--)
            {
                out_attr->cluster_list[i] = out_attr->cluster_list[i - 1];
            }
            memset(&out_attr->cluster_list[0], 0, sizeof(out_attr->cluster_list[0]));
            out_attr->cluster_list[0].family = AF_INET;
            out_attr->cluster_list[0].u.v4.s_addr = htonl(cid);
            out_attr->cluster_list_len++;
        }
    }

    if (stype == BGP_SESS_TYPE_EBGP)
    {
        uint32_t local_as = bgp_update_group_local_as();
        int n;
        if (BGP_ROUTE_ATTR(best)->as_path[0] != '\0')
        {
            n = g_snprintf(out_attr->as_path, sizeof(out_attr->as_path), "%u %s", local_as,
                           BGP_ROUTE_ATTR(best)->as_path);
        }
        else
        {
            n = g_snprintf(out_attr->as_path, sizeof(out_attr->as_path), "%u", local_as);
        }
        if (n < 0 || (size_t)n >= sizeof(out_attr->as_path))
        {
            LOG_WARN("BGP: subgroup eval_export AS_PATH prepend overflow ug_id=%u local_as=%u",
                     sg->parent ? sg->parent->group_id : 0u, local_as);
            return false;
        }
    }

    /* nexthop 策略应用 */
    return bgp_subgroup_apply_nexthop(sg, best, out_nh);
}

void bgp_update_group_enqueue_announce(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !nlri)
    {
        return;
    }

    bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, nlri);
    const bgp_route_node_t *best = rib ? bgp_rib_find_best(rib, nlri) : NULL;
    if (!best)
    {
        /* 无 best：走 withdraw 通路 */
        return;
    }

    /* NO_ADV 路径不允许对外通告：清理可能残留的 ARO 条目并发送 withdraw */
    if (BIT_TEST(best->flags, BGP_ROUTE_FLAG_NO_ADV))
    {
        bgp_update_group_enqueue_withdraw(inst, nlri);
        return;
    }

    bgp_route_src_class_t src_class = bgp_classify_route_src(best);

    uint32_t scheduled = 0;
    for (GList *ul = inst->update_groups; ul; ul = ul->next)
    {
        bgp_update_group_t *ug = (bgp_update_group_t *)ul->data;
        if (!ug)
        {
            continue;
        }

        /* 为本 (ug, route) 选择 rule；若拒绝（如 iBGP split-horizon），确保 ug
         * 内已有的 ARO 条目被撤销。 */
        bgp_nh_rule_t rule;
        bool accepted;
        if (inst->flags & BGP_INST_FLAG_NH_UNCHANGED)
        {
            rule = BGP_NH_RULE_PASS;
            accepted = true;
        }
        else
        {
            accepted = bgp_select_nh_rule(&ug->key, src_class, &rule);
        }
        if (!accepted)
        {
            /* 对本 ug 下所有子组尝试撤销该 NLRI（若存在于其中） */
            for (GList *sl = ug->subgroups; sl; sl = sl->next)
            {
                bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
                if (!sg || sg->peer_count == 0U || !sg->adj_rib_out)
                {
                    continue;
                }
                if (bgp_adj_rib_out_remove(sg->adj_rib_out, nlri))
                {
                    bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                    memcpy(copy, nlri, sizeof(*copy));
                    g_queue_push_tail(sg->withdraw_queue, copy);
                    sg->withdraw_count++;
                    scheduled++;
                }
            }
            continue;
        }

        /* 同一 rule 可能对应多个子组（不同 effective_local_addr）。
         * 对本 ug 下所有子组：rule 匹配则尝试发布，不匹配则撤销残留。 */
        for (GList *sl = ug->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (!sg || !sg->adj_rib_out)
            {
                continue;
            }

            if (sg->key.rule != rule)
            {
                /* 非目标 rule 的残留需撤销（例如 best 变化后 src_class 迁移） */
                if (bgp_adj_rib_out_remove(sg->adj_rib_out, nlri))
                {
                    bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                    memcpy(copy, nlri, sizeof(*copy));
                    g_queue_push_tail(sg->withdraw_queue, copy);
                    sg->withdraw_count++;
                    scheduled++;
                }
                continue;
            }

            if (sg->peer_count == 0U)
            {
                continue;
            }

            bgp_attr_t out_attr;
            bgp_nexthop_t out_nh;
            if (!bgp_subgroup_eval_export(sg, best, nlri, &out_attr, &out_nh))
            {
                if (bgp_adj_rib_out_remove(sg->adj_rib_out, nlri))
                {
                    bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                    memcpy(copy, nlri, sizeof(*copy));
                    g_queue_push_tail(sg->withdraw_queue, copy);
                    sg->withdraw_count++;
                    scheduled++;
                }
                continue;
            }

            if (ug->key.remote_as != 0U && bgp_attr_as_path_contains_as(out_attr.as_path, ug->key.remote_as))
            {
                if (bgp_adj_rib_out_remove(sg->adj_rib_out, nlri))
                {
                    bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                    memcpy(copy, nlri, sizeof(*copy));
                    g_queue_push_tail(sg->withdraw_queue, copy);
                    sg->withdraw_count++;
                    scheduled++;
                }
                continue;
            }

            bgp_attr_ref_t *ref = bgp_attr_intern(ug->inst, &out_attr);
            if (!ref)
            {
                continue;
            }

            bgp_aro_change_t ch = bgp_adj_rib_out_update(sg->adj_rib_out, nlri, ref, &out_nh);
            bgp_attr_release(ref); /* adj_rib_out 已持有自己的引用 */

            if (ch != BGP_ARO_UNCHANGED)
            {
                bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                memcpy(copy, nlri, sizeof(*copy));
                g_queue_push_tail(sg->announce_queue, copy);
                sg->announce_count++;
                scheduled++;
            }
        }
    }

    if (scheduled > 0u)
    {
        bgp_update_group_schedule_pub(inst);
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: enqueue announce to subgroups nlri=%s afi=%u safi=%u scheduled=%u", nlri_str,
                  (unsigned)inst->afi, (unsigned)inst->safi, scheduled);
    }
}

void bgp_update_group_enqueue_withdraw(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !nlri)
    {
        return;
    }

    uint32_t scheduled = 0;
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
            if (!sg || sg->peer_count == 0u || !sg->adj_rib_out)
            {
                continue;
            }

            if (bgp_adj_rib_out_remove(sg->adj_rib_out, nlri))
            {
                bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                memcpy(copy, nlri, sizeof(*copy));
                g_queue_push_tail(sg->withdraw_queue, copy);
                sg->withdraw_count++;
                scheduled++;
            }
        }
    }

    if (scheduled > 0u)
    {
        bgp_update_group_schedule_pub(inst);
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: enqueue withdraw to subgroups nlri=%s afi=%u safi=%u scheduled=%u", nlri_str,
                  (unsigned)inst->afi, (unsigned)inst->safi, scheduled);
    }
}

/** bgp_rib_foreach_best 回调：对每条 best 触发 announce 入队 */
static void catchup_populate_best_cb(const bgp_rthead_t *head, const bgp_route_node_t *route, gpointer user_data)
{
    (void)route;
    bgp_instance_t *inst = (bgp_instance_t *)user_data;
    if (!head || !inst)
    {
        return;
    }
    bgp_update_group_enqueue_announce(inst, &head->nlri);
}

/** bgp_adj_rib_out_foreach 回调：将 NLRI 重新推入该 subgroup 的 announce_queue（补发） */
static void catchup_requeue_aro_cb(const bgp_nlri_entry_t *nlri, const bgp_adj_rib_out_entry_t *entry,
                                   gpointer user_data)
{
    (void)entry;
    bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)user_data;
    if (!nlri || !sg || !sg->announce_queue)
    {
        return;
    }
    bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
    memcpy(copy, nlri, sizeof(*copy));
    g_queue_push_tail(sg->announce_queue, copy);
    sg->announce_count++;
}

void bgp_update_group_catchup_session(bgp_session_t *sess)
{
    if (!sess)
    {
        return;
    }

    uint32_t total_scheduled = 0;
    for (GList *l = sess->peer_list; l; l = l->next)
    {
        bgp_peer_t *peer = (bgp_peer_t *)l->data;
        if (!peer || !peer->inst)
        {
            continue;
        }
        bgp_instance_t *inst = peer->inst;

        /* peer 状态已在 fsm_on_established 中设置；未协商本 AF 的 peer 不加入子组、不回放 RIB */
        if (peer->state != BGP_PEER_STATE_ESTABLISHED)
        {
            continue;
        }

        /* 加入子组（幂等）；peer 可能归属多个 rule 的子组 */
        bgp_subgroup_peer_join(peer, sess);
        if (!peer->subgroups)
        {
            continue;
        }

        /* 统计是否所有子组都是空——是则做一次 full RIB 扫描；否则对非空子组做
         * ARO 补发。全量 RIB 扫描只需触发一次：enqueue_announce_to_subgroups 内部
         * 会分派到正确的 rule 子组。 */
        bool any_empty = false;
        bool any_populated = false;
        for (GList *sl = peer->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (!sg || !sg->adj_rib_out)
            {
                continue;
            }
            if (bgp_adj_rib_out_count(sg->adj_rib_out) == 0U)
            {
                any_empty = true;
            }
            else
            {
                any_populated = true;
            }
        }

        uint32_t before_total = 0;
        for (GList *sl = peer->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (sg)
            {
                before_total += sg->announce_count;
            }
        }

        if (any_empty && inst->rd_entries)
        {
            /* 跨所有 RD entry 注入 best-path 到 catchup */
            GHashTableIter rd_iter;
            gpointer rd_key, rd_val;
            g_hash_table_iter_init(&rd_iter, inst->rd_entries);
            while (g_hash_table_iter_next(&rd_iter, &rd_key, &rd_val))
            {
                (void)rd_key;
                bgp_rd_entry_t *e = (bgp_rd_entry_t *)rd_val;
                if (e && e->rib)
                {
                    bgp_rib_foreach_best(e->rib, catchup_populate_best_cb, inst);
                }
            }
        }
        if (any_populated)
        {
            for (GList *sl = peer->subgroups; sl; sl = sl->next)
            {
                bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
                if (sg && sg->adj_rib_out && bgp_adj_rib_out_count(sg->adj_rib_out) > 0U)
                {
                    bgp_adj_rib_out_foreach(sg->adj_rib_out, catchup_requeue_aro_cb, sg);
                }
            }
        }

        uint32_t after_total = 0;
        for (GList *sl = peer->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (sg)
            {
                after_total += sg->announce_count;
            }
        }
        if (after_total > before_total)
        {
            total_scheduled += (after_total - before_total);
            bgp_update_group_schedule_pub(inst);
        }
    }

    if (total_scheduled > 0u)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: neighbor=%s subgroup catchup scheduled %u NLRI(s) across %u peer(s)", addr_str, total_scheduled,
                 (unsigned)g_list_length(sess->peer_list));
    }
}

void bgp_update_group_refresh_session_af(bgp_session_t *sess, uint16_t afi, uint8_t safi)
{
    if (!sess)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    bgp_peer_t *target_peer = NULL;
    for (GList *l = sess->peer_list; l; l = l->next)
    {
        bgp_peer_t *peer = (bgp_peer_t *)l->data;
        if (!peer || !peer->inst)
        {
            continue;
        }
        if ((uint16_t)peer->inst->afi == afi && (uint8_t)peer->inst->safi == safi)
        {
            target_peer = peer;
            break;
        }
    }

    if (!target_peer)
    {
        LOG_WARN("BGP: neighbor=%s ROUTE-REFRESH ignored, AF=(%u/%u) not enabled", addr_str, afi, safi);
        return;
    }

    if (target_peer->state != BGP_PEER_STATE_ESTABLISHED)
    {
        LOG_WARN("BGP: neighbor=%s ROUTE-REFRESH ignored, AF=(%u/%u) peer not established", addr_str, afi, safi);
        return;
    }

    if (!target_peer->subgroups)
    {
        LOG_INFO("BGP: neighbor=%s ROUTE-REFRESH AF=(%u/%u) no subgroup membership, nothing to resend", addr_str, afi,
                 safi);
        return;
    }

    /* soft-out 语义：先走 RIB best 重 enqueue_announce 让 eval_export 用当前出向策略
     * （cluster-id / route-map 等）刷新 ARO 里的 attr_ref；然后强制把 ARO 内容全推到
     * announce_queue 触发重发——无条件 force-resend，不依赖 ARO 比对（attr 与上次相同
     * 也要发，否则配置未引起 attr 变更时对端拿不到新报文）。 */
    bgp_instance_t *inst = target_peer->inst;
    uint32_t before_total = 0;
    for (GList *sl = target_peer->subgroups; sl; sl = sl->next)
    {
        bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
        if (sg)
        {
            before_total += sg->announce_count;
        }
    }

    /* (1) RIB best → enqueue_announce：刷新 ARO（处理 attr 变更场景） */
    if (inst->rd_entries)
    {
        GHashTableIter rd_iter;
        gpointer rd_key = NULL;
        gpointer rd_val = NULL;
        g_hash_table_iter_init(&rd_iter, inst->rd_entries);
        while (g_hash_table_iter_next(&rd_iter, &rd_key, &rd_val))
        {
            (void)rd_key;
            bgp_rd_entry_t *e = (bgp_rd_entry_t *)rd_val;
            if (e && e->rib)
            {
                bgp_rib_foreach_best(e->rib, catchup_populate_best_cb, inst);
            }
        }
    }

    /* (2) ARO → announce_queue：强制把所有持有的条目重推一遍（force-resend） */
    for (GList *sl = target_peer->subgroups; sl; sl = sl->next)
    {
        bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
        if (sg && sg->adj_rib_out && bgp_adj_rib_out_count(sg->adj_rib_out) > 0U)
        {
            bgp_adj_rib_out_foreach(sg->adj_rib_out, catchup_requeue_aro_cb, sg);
        }
    }

    uint32_t after_total = 0;
    for (GList *sl = target_peer->subgroups; sl; sl = sl->next)
    {
        bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
        if (sg)
        {
            after_total += sg->announce_count;
        }
    }

    uint32_t scheduled = (after_total > before_total) ? (after_total - before_total) : 0u;
    if (scheduled > 0u)
    {
        bgp_update_group_schedule_pub(target_peer->inst);
        LOG_INFO("BGP: neighbor=%s ROUTE-REFRESH AF=(%u/%u) re-scheduled %u NLRI(s)", addr_str, afi, safi, scheduled);
    }
    else
    {
        LOG_INFO("BGP: neighbor=%s ROUTE-REFRESH AF=(%u/%u) Adj-RIB-Out empty, nothing to resend", addr_str, afi, safi);
    }
}

// ============================================================================
// Packed 发送辅助
// ============================================================================

/** 打包发布的单条 NLRI 及 per-session 过滤所需的来源信息 */
typedef struct announce_item
{
    const bgp_nlri_entry_t *nlri; /**< 借用指针（指向 g_queue 出队后堆上 NLRI 副本） */
    net_addr_t source;            /**< best->source（family=0 表示无来源/is_import） */
    bool is_import;               /**< best 是否为 IMPORT 路由（IMPORT 不做 split-horizon） */
} announce_item_t;

/** 按 (attr_ref, nexthop) 聚合的宣告桶 */
typedef struct announce_bucket
{
    bgp_attr_ref_t *attr_ref; /**< 借用（不增减引用） */
    bgp_nexthop_t nexthop;
    GPtrArray *items; /**< announce_item_t*（堆副本，调用方释放） */
} announce_bucket_t;

static announce_bucket_t *announce_bucket_find_or_create(GList **buckets, bgp_attr_ref_t *attr_ref,
                                                         const bgp_nexthop_t *nh)
{
    for (GList *l = *buckets; l; l = l->next)
    {
        announce_bucket_t *bk = (announce_bucket_t *)l->data;
        if (bk->attr_ref == attr_ref && memcmp(&bk->nexthop, nh, sizeof(*nh)) == 0)
        {
            return bk;
        }
    }
    announce_bucket_t *bk = g_new0(announce_bucket_t, 1);
    bk->attr_ref = attr_ref;
    bk->nexthop = *nh;
    bk->items = g_ptr_array_new();
    *buckets = g_list_prepend(*buckets, bk);
    return bk;
}

static void announce_bucket_free(announce_bucket_t *bk)
{
    if (!bk)
    {
        return;
    }
    if (bk->items)
    {
        for (guint i = 0; i < bk->items->len; i++)
        {
            announce_item_t *it = g_ptr_array_index(bk->items, i);
            g_free(it);
        }
        g_ptr_array_free(bk->items, TRUE);
    }
    g_free(bk);
}

/**
 * @brief 向单个 session 按 4096 字节报文上限循环发送 packed WITHDRAW
 */
static void subgroup_send_packed_withdraws(bgp_session_t *sess, uint16_t afi, uint8_t safi,
                                           const bgp_nlri_entry_t *const *nlri_list, int nlri_count)
{
    if (!sess || !sess->pri_conn || nlri_count <= 0)
    {
        return;
    }
    uint8_t msg[4096];
    int remaining = nlri_count;
    const bgp_nlri_entry_t *const *cursor = nlri_list;
    while (remaining > 0)
    {
        int packed = 0;
        int len =
            bgp_pkt_build_packed_withdraw(msg, (int)sizeof(msg), cursor, remaining, sess->pri_conn, afi, safi, &packed);
        if (len <= 0 || packed <= 0)
        {
            LOG_WARN("BGP: packed WITHDRAW build failed afi=%u safi=%u remaining=%d", (unsigned)afi, (unsigned)safi,
                     remaining);
            break;
        }
        ssize_t sent = send(sess->pri_conn->fd, msg, (size_t)len, MSG_NOSIGNAL);
        if (sent != (ssize_t)len)
        {
            LOG_WARN("BGP: packed WITHDRAW send incomplete (sent=%zd want=%d)", sent, len);
            break;
        }
        bgp_session_tx_msg_count(sess, BGP_MSG_UPDATE);
        cursor += packed;
        remaining -= packed;
    }
}

void bgp_send_packed_withdraws_to_session(bgp_session_t *sess, uint16_t afi, uint8_t safi,
                                          const bgp_nlri_entry_t *const *nlri_list, int nlri_count)
{
    subgroup_send_packed_withdraws(sess, afi, safi, nlri_list, nlri_count);
}

/**
 * @brief 向单个 session 按 4096 字节报文上限循环发送 packed UPDATE
 */
static void subgroup_send_packed_updates(bgp_session_t *sess, uint16_t afi, uint8_t safi,
                                         const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                         const bgp_attr_t *attr, const bgp_nexthop_t *nh)
{
    if (!sess || !sess->pri_conn || nlri_count <= 0 || !attr || !nh)
    {
        return;
    }
    uint8_t msg[4096];
    int remaining = nlri_count;
    const bgp_nlri_entry_t *const *cursor = nlri_list;
    while (remaining > 0)
    {
        int packed = 0;
        int len = bgp_pkt_build_packed_update(msg, (int)sizeof(msg), cursor, remaining, attr, nh, afi, safi, &packed);
        if (len <= 0 || packed <= 0)
        {
            LOG_WARN("BGP: packed UPDATE build failed afi=%u safi=%u remaining=%d", (unsigned)afi, (unsigned)safi,
                     remaining);
            break;
        }
        ssize_t sent = send(sess->pri_conn->fd, msg, (size_t)len, MSG_NOSIGNAL);
        if (sent != (ssize_t)len)
        {
            LOG_WARN("BGP: packed UPDATE send incomplete (sent=%zd want=%d)", sent, len);
            break;
        }
        bgp_session_tx_msg_count(sess, BGP_MSG_UPDATE);
        cursor += packed;
        remaining -= packed;
    }
}

/**
 * @brief 组包一次，多播到 peer_list 中所有 publish-ready 的 session
 *
 * 同 UG 内 remote_as / negotiated_caps / peer_family 已一致，意味着
 * (attr, nh) 确定后打包结果也确定，可复用同一份字节流发给多个 peer。
 * 调用者需保证这批 NLRI 对 peer_list 中任一 peer 都不触发 split-horizon。
 */
static void subgroup_pack_and_multicast(GList *peer_list, uint16_t afi, uint8_t safi,
                                        const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                        const bgp_attr_t *attr, const bgp_nexthop_t *nh)
{
    if (!peer_list || nlri_count <= 0 || !attr || !nh)
    {
        return;
    }

    /* 先收集 ready session，避免循环中重复查找 */
    GPtrArray *sessions = g_ptr_array_new();
    for (GList *l = peer_list; l; l = l->next)
    {
        bgp_peer_t *peer = (bgp_peer_t *)l->data;
        if (!peer || !peer->vrf)
        {
            continue;
        }
        bgp_session_t *sess = bgp_vrf_find_session(peer->vrf, &peer->addr);
        if (!bgp_session_is_publish_ready(sess))
        {
            continue;
        }
        g_ptr_array_add(sessions, sess);
    }
    if (sessions->len == 0)
    {
        g_ptr_array_free(sessions, TRUE);
        return;
    }

    uint8_t msg[4096];
    int remaining = nlri_count;
    const bgp_nlri_entry_t *const *cursor = nlri_list;
    while (remaining > 0)
    {
        int packed = 0;
        int len = bgp_pkt_build_packed_update(msg, (int)sizeof(msg), cursor, remaining, attr, nh, afi, safi, &packed);
        if (len <= 0 || packed <= 0)
        {
            LOG_WARN("BGP: packed UPDATE build failed afi=%u safi=%u remaining=%d", (unsigned)afi, (unsigned)safi,
                     remaining);
            break;
        }
        for (guint i = 0; i < sessions->len; i++)
        {
            bgp_session_t *sess = (bgp_session_t *)g_ptr_array_index(sessions, i);
            if (!sess || !sess->pri_conn)
            {
                continue;
            }
            ssize_t sent = send(sess->pri_conn->fd, msg, (size_t)len, MSG_NOSIGNAL);
            if (sent != (ssize_t)len)
            {
                LOG_WARN("BGP: packed UPDATE multicast send incomplete (sent=%zd want=%d)", sent, len);
                continue;
            }
            bgp_session_tx_msg_count(sess, BGP_MSG_UPDATE);
        }
        cursor += packed;
        remaining -= packed;
    }

    g_ptr_array_free(sessions, TRUE);
}

int bgp_subgroup_process_queues(bgp_nh_subgroup_t *sg, bgp_instance_t *inst, int batch_size)
{
    if (!sg || !inst || batch_size <= 0)
    {
        return 0;
    }

    uint16_t afi = (uint16_t)inst->afi;
    uint8_t safi = (uint8_t)inst->safi;
    int processed = 0;

    /* --- Packed WITHDRAW --- */
    GPtrArray *wd_nlris = g_ptr_array_new();
    while (processed < batch_size && sg->withdraw_queue && !g_queue_is_empty(sg->withdraw_queue))
    {
        bgp_nlri_entry_t *nlri = (bgp_nlri_entry_t *)g_queue_pop_head(sg->withdraw_queue);
        if (!nlri)
        {
            break;
        }
        g_ptr_array_add(wd_nlris, nlri);
        processed++;
    }
    if (wd_nlris->len > 0)
    {
        for (GList *l = sg->peer_list; l; l = l->next)
        {
            bgp_peer_t *peer = (bgp_peer_t *)l->data;
            if (!peer || !peer->vrf)
            {
                continue;
            }
            bgp_session_t *sess = bgp_vrf_find_session(peer->vrf, &peer->addr);
            if (!bgp_session_is_publish_ready(sess))
            {
                continue;
            }
            subgroup_send_packed_withdraws(sess, afi, safi, (const bgp_nlri_entry_t *const *)wd_nlris->pdata,
                                           (int)wd_nlris->len);
        }
    }
    for (guint i = 0; i < wd_nlris->len; i++)
    {
        g_free(g_ptr_array_index(wd_nlris, i));
    }
    g_ptr_array_free(wd_nlris, TRUE);

    /* --- Packed ANNOUNCE：按 (attr_ref, nh) 分桶 --- */
    GList *buckets = NULL;
    GPtrArray *ann_nlris = g_ptr_array_new(); /* 记录所有出队 NLRI 堆副本（最终统一释放） */
    while (processed < batch_size && sg->announce_queue && !g_queue_is_empty(sg->announce_queue))
    {
        bgp_nlri_entry_t *nlri = (bgp_nlri_entry_t *)g_queue_pop_head(sg->announce_queue);
        if (!nlri)
        {
            break;
        }
        processed++;
        g_ptr_array_add(ann_nlris, nlri);

        const bgp_adj_rib_out_entry_t *entry = bgp_adj_rib_out_lookup(sg->adj_rib_out, nlri);
        if (!entry || !entry->attr_ref)
        {
            continue;
        }
        bgp_rib_t *rib_for_nlri = bgp_inst_rib_for_nlri(inst, nlri);
        const bgp_route_node_t *best = rib_for_nlri ? bgp_rib_find_best(rib_for_nlri, nlri) : NULL;

        /* vpnv4 本地导出路由：loc-rib 不带标签，发送时按 per-vrf 申请标签注入 NLRI;
         * 申请不到则 hold（本次不通告），待标签可得后下次 pub 重试。 */
        if (best && inst->safi == BGP_SAFI_VPN_UNICAST && bgp_route_is_local_origin(best))
        {
            uint32_t label = bgp_vrf_export_resolve_send_label(best);
            if (label == 0u)
            {
                continue; /* hold：nlri 堆副本最终随 ann_nlris 统一释放 */
            }
            nlri->prefix.label = label;
            nlri->prefix.has_label = true;
        }

        announce_bucket_t *bk = announce_bucket_find_or_create(&buckets, entry->attr_ref, &entry->nexthop);

        announce_item_t *item = g_new0(announce_item_t, 1);
        item->nlri = nlri;
        if (best)
        {
            item->source = best->source;
            item->is_import = bgp_route_is_local_origin(best);
        }
        else
        {
            item->source.family = 0;
            item->is_import = false;
        }
        g_ptr_array_add(bk->items, item);
    }

    /* 遍历桶并发送：UG 键已保证 remote_as/negotiated_caps 一致，
     * 因此 AS_PATH 防环在桶级一次判定、无 split-horizon 时组包一次多播 */
    bgp_update_group_t *ug = sg->parent;
    for (GList *bl = buckets; bl; bl = bl->next)
    {
        announce_bucket_t *bk = (announce_bucket_t *)bl->data;
        if (!bk || !bk->attr_ref || !bk->items || bk->items->len == 0)
        {
            continue;
        }

        /* AS_PATH 防环：同 UG 共用 remote_as，桶级一次判定 */
        gboolean as_loop = FALSE;
        if (ug && ug->key.remote_as != 0U &&
            bgp_attr_as_path_contains_as(bk->attr_ref->attr.as_path, ug->key.remote_as))
        {
            as_loop = TRUE;
        }

        if (!as_loop)
        {
            /* 判定是否存在 split-horizon：任一 NLRI 来源命中任一 peer 地址 */
            gboolean need_per_peer = FALSE;
            for (guint i = 0; i < bk->items->len && !need_per_peer; i++)
            {
                announce_item_t *it = g_ptr_array_index(bk->items, i);
                if (it->is_import || it->source.family == 0)
                {
                    continue;
                }
                for (GList *sl = sg->peer_list; sl; sl = sl->next)
                {
                    bgp_peer_t *peer = (bgp_peer_t *)sl->data;
                    if (peer && net_addr_equal(&it->source, &peer->addr))
                    {
                        need_per_peer = TRUE;
                        break;
                    }
                }
            }

            if (!need_per_peer)
            {
                /* 快路径：组包一次，多播到组内所有 peer */
                GPtrArray *nlris = g_ptr_array_new();
                for (guint i = 0; i < bk->items->len; i++)
                {
                    announce_item_t *it = g_ptr_array_index(bk->items, i);
                    g_ptr_array_add(nlris, (gpointer)it->nlri);
                }
                subgroup_pack_and_multicast(sg->peer_list, afi, safi, (const bgp_nlri_entry_t *const *)nlris->pdata,
                                            (int)nlris->len, &bk->attr_ref->attr, &bk->nexthop);
                g_ptr_array_free(nlris, TRUE);
            }
            else
            {
                /* 慢路径：逐 peer 过滤 split-horizon */
                for (GList *sl = sg->peer_list; sl; sl = sl->next)
                {
                    bgp_peer_t *peer = (bgp_peer_t *)sl->data;
                    if (!peer || !peer->vrf)
                    {
                        continue;
                    }
                    bgp_session_t *sess = bgp_vrf_find_session(peer->vrf, &peer->addr);
                    if (!bgp_session_is_publish_ready(sess))
                    {
                        continue;
                    }
                    GPtrArray *filtered = g_ptr_array_new();
                    for (guint i = 0; i < bk->items->len; i++)
                    {
                        announce_item_t *it = g_ptr_array_index(bk->items, i);
                        if (!it->is_import && it->source.family != 0 &&
                            net_addr_equal(&it->source, &sess->neighbor_addr))
                        {
                            continue;
                        }
                        g_ptr_array_add(filtered, (gpointer)it->nlri);
                    }
                    if (filtered->len > 0)
                    {
                        subgroup_send_packed_updates(sess, afi, safi, (const bgp_nlri_entry_t *const *)filtered->pdata,
                                                     (int)filtered->len, &bk->attr_ref->attr, &bk->nexthop);
                    }
                    g_ptr_array_free(filtered, TRUE);
                }
            }
        }

        /* 标记 advertised（无论是否因 AS_PATH 环路被跳过，均避免重复入队） */
        for (guint i = 0; i < bk->items->len; i++)
        {
            announce_item_t *it = g_ptr_array_index(bk->items, i);
            bgp_adj_rib_out_mark_advertised(sg->adj_rib_out, it->nlri);
        }
    }

    /* 清理 */
    for (GList *bl = buckets; bl; bl = bl->next)
    {
        announce_bucket_free((announce_bucket_t *)bl->data);
    }
    g_list_free(buckets);
    for (guint i = 0; i < ann_nlris->len; i++)
    {
        g_free(g_ptr_array_index(ann_nlris, i));
    }
    g_ptr_array_free(ann_nlris, TRUE);

    return processed;
}

// ============================================================================
// 事件派发 / 调度
// ============================================================================

static int bgp_update_group_process_pub_event(bgp_instance_t *inst, gboolean allow_reschedule)
{
    if (!inst)
    {
        return 0;
    }

    int total_processed = 0;
    gboolean need_more = FALSE;

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
            int processed = bgp_subgroup_process_queues(sg, inst, BGP_WORK_BATCH_SIZE);
            if (processed > 0)
            {
                total_processed += processed;
            }
            if ((sg->announce_queue && !g_queue_is_empty(sg->announce_queue)) ||
                (sg->withdraw_queue && !g_queue_is_empty(sg->withdraw_queue)))
            {
                need_more = TRUE;
            }
        }
    }

    if (allow_reschedule && total_processed > 0 && need_more)
    {
        bgp_update_group_schedule_pub(inst);
    }

    return total_processed;
}

static void bgp_update_group_schedule_pub(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : BGP_VRF_PUBLIC_ID;
    if (bgp_worker_post_session_pub_event(vrf_id, inst->afi, inst->safi) == 0)
    {
        return;
    }

    if (bgp_worker_is_current_thread())
    {
        (void)bgp_update_group_process_pub_event(inst, FALSE);
        return;
    }

    LOG_WARN("BGP: failed to enqueue session-pub work event vrf=%u afi=%u safi=%u", vrf_id, (unsigned)inst->afi,
             (unsigned)inst->safi);
}

void bgp_update_group_handle_pub_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_instance_t *inst = bgp_worker_lookup_instance(vrf_id, afi, safi);
    (void)bgp_update_group_process_pub_event(inst, TRUE);
}

int bgp_update_group_process_pending(bgp_instance_t *inst)
{
    return bgp_update_group_process_pub_event(inst, FALSE);
}
