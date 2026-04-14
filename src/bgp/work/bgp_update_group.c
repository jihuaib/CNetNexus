/**
 * @file   bgp_update_group.c
 * @brief  BGP Update Group / NH Subgroup 生命周期与 peer 归组
 * @author jhb
 * @date   2026/04/10
 */
#include "bgp_update_group.h"

#include <string.h>

#include "bgp_adj_rib_out.h"
#include "bgp_conn.h"
#include "bgp_instance.h"
#include "bgp_peer.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"
#include "log.h"

/** 自增 group_id，仅用于日志/调试 */
static uint32_t g_next_group_id = 1;

/* ============================================================================
 * 键相等判断
 * ========================================================================== */

static gboolean ug_key_equal(const bgp_update_group_key_t *a, const bgp_update_group_key_t *b)
{
    return a->sess_type == b->sess_type && a->policy_hash == b->policy_hash && a->peer_family == b->peer_family;
}

static gboolean sg_key_equal(const bgp_nh_subgroup_key_t *a, const bgp_nh_subgroup_key_t *b)
{
    if (a->rule != b->rule)
    {
        return FALSE;
    }
    /* 只有 R_LOCAL 使用 local_addr，其它 rule 下 family=0 自动相等 */
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
    if (BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT))
    {
        return BGP_RSRC_IMPORT;
    }
    /* 非 IMPORT 路由：根据源 session 类型分类 */
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
            /* iBGP split-horizon：非 RR 场景下不转发 iBGP-learned 给 iBGP */
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
    bgp_session_compute_ug_key(sess, &ugk);
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
    int nrules = compute_rules_for_ug(&ugk, rules, (int)(sizeof(rules) / sizeof(rules[0])));

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
