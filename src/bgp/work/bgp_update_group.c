/**
 * @file   bgp_update_group.c
 * @brief  BGP Update Group / NH Subgroup 生命周期与 session 归组
 * @author jhb
 * @date   2026/04/10
 */
#include "bgp_update_group.h"

#include <string.h>

#include "bgp_adj_rib_out.h"
#include "bgp_conn.h"
#include "bgp_instance.h"
#include "log.h"

/** 自增 group_id，仅用于日志/调试 */
static uint32_t g_next_group_id = 1;

/* ============================================================================
 * 键相等判断
 * ========================================================================== */

static gboolean ug_key_equal(const bgp_update_group_key_t *a, const bgp_update_group_key_t *b)
{
    return a->sess_type == b->sess_type && a->policy_hash == b->policy_hash;
}

static gboolean sg_key_equal(const bgp_nh_subgroup_key_t *a, const bgp_nh_subgroup_key_t *b)
{
    if (a->nh_policy != b->nh_policy)
    {
        return FALSE;
    }
    return net_addr_equal(&a->effective_local_addr, &b->effective_local_addr);
}

/* ============================================================================
 * 键计算
 * ========================================================================== */

void bgp_session_compute_ug_key(const bgp_session_t *sess, bgp_update_group_key_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sess_type = sess ? sess->sess_type : BGP_SESS_TYPE_UNKNOWN;
    out->policy_hash = 0; /* 预留：Phase 4 引入 route-map 时填入 */
}

void bgp_session_compute_sg_key(const bgp_session_t *sess, bgp_nh_subgroup_key_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!sess)
    {
        return;
    }

    /* 默认 nh 策略：eBGP=SELF，iBGP=UNCHANGED。后续可扩展配置覆盖。 */
    if (sess->sess_type == BGP_SESS_TYPE_EBGP)
    {
        out->nh_policy = BGP_NH_POLICY_SELF;
    }
    else
    {
        out->nh_policy = BGP_NH_POLICY_UNCHANGED;
    }

    /* 缓存本端连接地址（ESTABLISHED 时有效；否则 family=0） */
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
    sg->session_list = NULL;
    sg->session_count = 0;
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
    /* session_list 为借用引用，不持有所有权；只释放链表节点本身 */
    g_list_free(sg->session_list);
    sg->session_list = NULL;

    /* announce_queue / withdraw_queue 内部元素为 bgp_nlri_entry_t 堆副本（Phase 2 中入队） */
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

void bgp_nh_subgroup_remove_if_empty(bgp_update_group_t *ug, bgp_nh_subgroup_t *sg)
{
    if (!ug || !sg)
    {
        return;
    }
    if (sg->session_count > 0)
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
    LOG_DEBUG("BGP: update_group created id=%u sess_type=%d afi=%u safi=%u", ug->group_id, (int)key->sess_type,
              (unsigned)inst->afi, (unsigned)inst->safi);
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
 * Session 加入/离开
 * ========================================================================== */

void bgp_subgroup_session_join(bgp_instance_t *inst, bgp_session_t *sess)
{
    if (!inst || !sess)
    {
        return;
    }
    /* 幂等：已在某 subgroup 中，先 leave */
    if (sess->subgroup)
    {
        bgp_subgroup_session_leave(inst, sess);
    }

    bgp_update_group_key_t ugk;
    bgp_nh_subgroup_key_t sgk;
    bgp_session_compute_ug_key(sess, &ugk);
    bgp_session_compute_sg_key(sess, &sgk);

    bgp_update_group_t *ug = bgp_update_group_find_or_create(inst, &ugk);
    bgp_nh_subgroup_t *sg = bgp_nh_subgroup_find_or_create(ug, &sgk);
    sg->session_list = g_list_append(sg->session_list, sess);
    sg->session_count++;
    sess->subgroup = sg;

    char addr_buf[64];
    net_addr_to_str(&sess->neighbor_addr, addr_buf, sizeof(addr_buf));
    LOG_DEBUG("BGP: session %s join subgroup (ug_id=%u nh_policy=%d) afi=%u safi=%u", addr_buf, ug->group_id,
              (int)sgk.nh_policy, (unsigned)inst->afi, (unsigned)inst->safi);
}

void bgp_subgroup_session_leave(bgp_instance_t *inst, bgp_session_t *sess)
{
    if (!inst || !sess || !sess->subgroup)
    {
        return;
    }
    bgp_nh_subgroup_t *sg = sess->subgroup;
    bgp_update_group_t *ug = sg->parent;

    sg->session_list = g_list_remove(sg->session_list, sess);
    if (sg->session_count > 0)
    {
        sg->session_count--;
    }
    sess->subgroup = NULL;

    char addr_buf[64];
    net_addr_to_str(&sess->neighbor_addr, addr_buf, sizeof(addr_buf));
    LOG_DEBUG("BGP: session %s leave subgroup (ug_id=%u) afi=%u safi=%u", addr_buf, ug ? ug->group_id : 0,
              (unsigned)inst->afi, (unsigned)inst->safi);

    /* 空子组清理；子组清理后空组也清理 */
    if (sg->session_count == 0)
    {
        bgp_nh_subgroup_remove_if_empty(ug, sg);
        if (ug && ug->subgroup_count == 0)
        {
            bgp_update_group_remove_if_empty(inst, ug);
        }
    }
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
