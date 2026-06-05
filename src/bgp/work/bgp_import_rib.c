/**
 * @file   bgp_import_rib.c
 * @brief  BGP 跨 AF 路由互导（import-rib）实现：mirror 拷贝、refcount、
 *         pending queue、级联 free、隧道迭代下刷
 * @author jhb
 * @date   2026/05/17
 */
#include "bgp_import_rib.h"

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bgp.h"
#include "bgp_attr_intern.h"
#include "bgp_calc.h"
#include "bgp_instance.h"
#include "bgp_nexthop.h"
#include "bgp_protocol.h"
#include "bgp_rd.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "bit.h"
#include "log.h"
#include "vrf.h"

/**
 * @brief 单个 unicast inst 的 import-rib 状态（挂在 inst->import_rib_state）
 */
typedef struct bgp_import_rib_state
{
    GQueue *pending;           /**< 待处理的源 rthead 队列（入队 ref，出队 unref） */
    uint32_t pending_count;    /**< pending 队列长度 */
    GHashTable *mirror_by_src; /**< key = 源 bgp_route_node_t*，value = mirror bgp_route_node_t* */
    bool no_route_flush;       /**< 该 inst 是否跳过 ROUTE 下刷（labeled 置位） */
} bgp_import_rib_state_t;

/* ============================================================================
 * 内部工具
 * ========================================================================== */

/**
 * @brief 根据源 instance 查找当前 import-rib 的目标 instance（按源 safi 派发）
 *
 * 第一阶段：源 safi=LABELED → 目标为同 vrf 同 afi 的 unicast；仅在目标 inst 已使能对应
 * import 源时返回非空。后续支持 vpn-instance / vpn-unicast 时在此函数内扩展派发表。
 */
static bgp_instance_t *bgp_import_rib_find_target_inst(bgp_instance_t *src_inst)
{
    if (!src_inst || !src_inst->vrf)
    {
        return NULL;
    }
    if (src_inst->safi != BGP_SAFI_LABELED)
    {
        return NULL;
    }
    bgp_instance_t *tgt_inst = (bgp_instance_t *)g_hash_table_lookup(
        src_inst->vrf->inst_hash, bgp_inst_hash_key(src_inst->afi, BGP_SAFI_UNICAST));
    if (!tgt_inst || !tgt_inst->import_rib_state)
    {
        return NULL;
    }
    if ((tgt_inst->import_rib_sources & (1U << BGP_IMPORT_SRC_LABELED_UC)) == 0U)
    {
        return NULL;
    }
    return tgt_inst;
}

/**
 * @brief 由源 NLRI 推导目标 NLRI（按当前 import 源类型剥离 RD/label 等）
 */
static void bgp_import_rib_derive_target_nlri(const bgp_nlri_entry_t *src, bgp_nlri_entry_t *dst)
{
    *dst = *src;
    dst->safi = BGP_SAFI_UNICAST;
    if (dst->type == BGP_NLRI_PREFIX)
    {
        dst->prefix.has_label = false;
        dst->prefix.label = 0;
    }
}

/**
 * @brief 将 mirror 节点字段从源节点同步过来（attr/label/时间戳）
 *
 * 不触碰 borrow_refcnt、src_route、flags 中的 IMPORT_RIB；这些由调用方在 create/复用时维护。
 */
static void bgp_import_rib_mirror_sync_from_src(bgp_route_node_t *mirror, const bgp_route_node_t *src)
{
    if (!mirror || !src)
    {
        return;
    }
    if (src->attr != mirror->attr)
    {
        bgp_attr_ref_get((bgp_attr_ref_t *)src->attr);
        bgp_attr_release(mirror->attr);
        mirror->attr = src->attr;
    }
    bgp_nexthop_reset_route(mirror);
    mirror->label = src->label;
    mirror->has_label = src->has_label;
    mirror->label_source = src->label_source;
    mirror->updated_at_usec = g_get_real_time();
}

/**
 * @brief 在目标 head 下创建或复用 mirror 节点，并建立 src→mirror 反向映射
 *
 * 关键去重：上层 bgp_rthead_create_route 不去重 source，会无条件 append；这里先
 * lookup_route_mut(target_head, src->source)，命中则复用、原地刷新，避免在
 * target_head->route_list 下堆积同 source 的幽灵 IMPORT_RIB 节点（曾导致 UAF：
 * mirror_withdraw 时 borrow_unref 被对同一 src 多次调用）。
 */
static bgp_route_node_t *bgp_import_rib_mirror_create(bgp_instance_t *tgt_inst, bgp_rib_t *tgt_rib,
                                                      bgp_rthead_t *tgt_head, const bgp_nlri_entry_t *tgt_nlri,
                                                      bgp_route_node_t *src)
{
    if (!tgt_inst || !tgt_rib || !tgt_head || !src)
    {
        return NULL;
    }
    bgp_import_rib_state_t *st = (bgp_import_rib_state_t *)tgt_inst->import_rib_state;
    if (!st)
    {
        return NULL;
    }

    /* 去重 lookup：同 (tgt_head, src->source) 已有节点则复用 */
    bgp_route_node_t *mirror = bgp_rthead_lookup_route_mut(tgt_head, &src->source);
    gboolean newly_created = FALSE;
    if (!mirror)
    {
        mirror = bgp_rthead_create_route(tgt_rib, tgt_head, &src->source);
        if (!mirror)
        {
            return NULL;
        }
        newly_created = TRUE;
        mirror->added_at_usec = g_get_real_time();
        mirror->borrow_refcnt = 0;
        mirror->src_route = NULL;
    }

    /* 复用：若已有 src_route 指向其他 src，先解除旧的借用 */
    if (mirror->src_route && mirror->src_route != src)
    {
        bgp_route_node_t *old_src = mirror->src_route;
        g_hash_table_remove(st->mirror_by_src, old_src);
        bgp_route_node_borrow_unref(old_src);
        mirror->src_route = NULL;
    }

    bgp_import_rib_mirror_sync_from_src(mirror, src);

    BIT_SET(mirror->flags, BGP_ROUTE_FLAG_VALID);
    BIT_SET(mirror->flags, BGP_ROUTE_FLAG_IMPORT_RIB);
    BIT_CLR(mirror->flags, BGP_ROUTE_FLAG_BEST);
    BIT_CLR(mirror->flags, BGP_ROUTE_FLAG_FLUSHED);
    BIT_CLR(mirror->flags, BGP_ROUTE_FLAG_STALE);

    if (mirror->src_route != src)
    {
        mirror->src_route = src;
        bgp_route_node_borrow_ref(src);
        g_hash_table_insert(st->mirror_by_src, src, mirror);
    }

    if (tgt_inst->calc_queue && tgt_nlri)
    {
        bgp_calc_queue_push(tgt_inst->calc_queue, tgt_inst, tgt_nlri);
    }

    char nbuf[BGP_NLRI_KEY_MAX];
    bgp_nlri_to_str(tgt_nlri, nbuf, sizeof(nbuf));
    LOG_DEBUG("BGP import-rib: mirror %s nlri=%s src=%p refcnt=%u", newly_created ? "created" : "updated", nbuf,
              (void *)src, src->borrow_refcnt);
    return mirror;
}

/**
 * @brief 撤销 mirror：从目标 RIB 摘除，并对 src 减计数（必要时触发 src 彻底释放）
 */
static void bgp_import_rib_mirror_withdraw(bgp_instance_t *tgt_inst, bgp_rib_t *tgt_rib,
                                           const bgp_nlri_entry_t *tgt_nlri, bgp_route_node_t *mirror)
{
    if (!tgt_inst || !tgt_rib || !tgt_nlri || !mirror)
    {
        return;
    }
    bgp_import_rib_state_t *st = (bgp_import_rib_state_t *)tgt_inst->import_rib_state;
    if (!st)
    {
        return;
    }

    bgp_route_node_t *src = mirror->src_route;
    net_addr_t src_source = mirror->source;

    if (src)
    {
        g_hash_table_remove(st->mirror_by_src, src);
    }

    (void)bgp_rib_unreach_one(tgt_rib, tgt_nlri, &src_source);
    if (tgt_inst->calc_queue)
    {
        bgp_calc_queue_push(tgt_inst->calc_queue, tgt_inst, tgt_nlri);
    }

    if (src)
    {
        bgp_route_node_borrow_unref(src);
    }
}

/* ============================================================================
 * 模块/实例 生命周期
 * ========================================================================== */

int bgp_import_rib_init(void)
{
    return 0;
}

void bgp_import_rib_inst_init(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }
    bgp_import_rib_state_t *st = g_new0(bgp_import_rib_state_t, 1);
    st->pending = g_queue_new();
    st->pending_count = 0;
    st->mirror_by_src = g_hash_table_new(g_direct_hash, g_direct_equal);
    /* labeled instance 不下刷 ROUTE */
    st->no_route_flush = (inst->safi == BGP_SAFI_LABELED);
    inst->import_rib_state = st;
}

void bgp_import_rib_inst_destroy(bgp_instance_t *inst)
{
    if (!inst || !inst->import_rib_state)
    {
        return;
    }
    bgp_import_rib_state_t *st = (bgp_import_rib_state_t *)inst->import_rib_state;

    /* 1. 抽干 pending 队列（unref 所有 head） */
    if (st->pending)
    {
        bgp_rthead_t *head = NULL;
        while ((head = (bgp_rthead_t *)g_queue_pop_head(st->pending)) != NULL)
        {
            bgp_rib_head_unref(head);
        }
        g_queue_free(st->pending);
        st->pending = NULL;
    }
    /* 2. 撤销所有 mirror —— 但此时 inst 即将被销毁，整个 RIB 也将随之释放，
     *    我们只需要清空 mirror_by_src 并对源做 borrow_unref（让源能够正常 free）。 */
    if (st->mirror_by_src)
    {
        GHashTableIter iter;
        gpointer src_p = NULL;
        gpointer mirror_p = NULL;
        g_hash_table_iter_init(&iter, st->mirror_by_src);
        while (g_hash_table_iter_next(&iter, &src_p, &mirror_p))
        {
            bgp_route_node_borrow_unref((bgp_route_node_t *)src_p);
        }
        g_hash_table_destroy(st->mirror_by_src);
        st->mirror_by_src = NULL;
    }
    g_free(st);
    inst->import_rib_state = NULL;
}

/* ============================================================================
 * 钩子 API
 * ========================================================================== */

bool bgp_import_rib_should_skip_flush(const bgp_instance_t *inst)
{
    if (!inst || !inst->import_rib_state)
    {
        return false;
    }
    const bgp_import_rib_state_t *st = (const bgp_import_rib_state_t *)inst->import_rib_state;
    return st->no_route_flush;
}

bool bgp_import_rib_is_mirror(const bgp_route_node_t *route)
{
    if (!route)
    {
        return false;
    }
    return BIT_TEST(route->flags, BGP_ROUTE_FLAG_IMPORT_RIB) != 0;
}

int bgp_import_rib_tiebreak(const bgp_route_node_t *cand, const bgp_route_node_t *cur)
{
    if (!cand || !cur)
    {
        return 0;
    }
    bool cand_mirror = bgp_import_rib_is_mirror(cand);
    bool cur_mirror = bgp_import_rib_is_mirror(cur);
    if (cand_mirror == cur_mirror)
    {
        return 0;
    }
    /* mirror（隧道迭代）输给非 mirror（IP 迭代） */
    return cand_mirror ? -1 : 1;
}

void bgp_import_rib_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head, const bgp_route_node_t *old_best,
                                 const bgp_route_node_t *new_best)
{
    (void)old_best;
    (void)new_best;
    if (!src_inst || !head)
    {
        return;
    }
    bgp_instance_t *tgt_inst = bgp_import_rib_find_target_inst(src_inst);
    if (!tgt_inst || !tgt_inst->import_rib_state)
    {
        return;
    }
    bgp_import_rib_state_t *st = (bgp_import_rib_state_t *)tgt_inst->import_rib_state;
    bgp_rib_head_ref(head);
    g_queue_push_tail(st->pending, head);
    st->pending_count++;
}

int bgp_import_rib_flush_mirror(dev_ipc_context_t *ctx, uint32_t vrf_id, const bgp_nlri_entry_t *nlri,
                                bgp_route_node_t *mirror, bool withdraw)
{
    (void)ctx;
    (void)vrf_id;
    (void)nlri;
    (void)mirror;
    (void)withdraw;
    /* 下刷路径：mirror 通过 src_route 复用源路径的 nexthop/tunnel value；bgp_route_flush 的
     * route_node_to_route_entry 会根据该 value 自动选择 nh_type=TUNNEL。
     * 因此这里返回 0 表示"按标准 flush 流程下刷"，避免重复路径。
     * （真正的隧道注册由 labeled 侧 bgp_relay 完成，mirror 仅复用结果。） */
    return 0;
}

/* ============================================================================
 * pending queue 处理
 * ========================================================================== */

/**
 * @brief 处理 pending 中的一个源 head：查当前 best 并 reconcile 到目标 RIB 的 mirror
 *
 * 上层不区分 same-src/diff-src/不存在；统一调 mirror_create，由其内部 lookup_route_mut 去重：
 *   - 命中：复用并原地刷新（含 src_route 切换时的旧借用解除）
 *   - 未命中：g_malloc0 新节点并 append
 */
static int bgp_import_rib_process_one(bgp_instance_t *tgt_inst, bgp_rthead_t *src_head)
{
    if (!tgt_inst || !src_head)
    {
        return 0;
    }
    if (!tgt_inst->import_rib_state)
    {
        return 0;
    }

    bgp_instance_t *src_inst = src_head->inst;
    if (!src_inst)
    {
        return 0;
    }

    /* 源 best */
    bgp_rib_t *src_rib = bgp_inst_rib_for_nlri(src_inst, &src_head->nlri);
    const bgp_route_node_t *src_best = src_rib ? bgp_rib_find_best(src_rib, &src_head->nlri) : NULL;

    /* 仅镜像 peer 路由：本地 import-route 引入的 best（带 BGP_ROUTE_FLAG_IMPORT）已经被
     * `bgp_import_route_entry_to_safi` 同步灌进目标 unicast inst，重复 mirror 不仅多余，
     * 还会因 source 与 import-route 节点重合而污染 unicast RIB（lookup_route_mut 按 source 命中后
     * 会被 mirror_create 当成 existing mirror 改写）。 */
    if (src_best && BIT_TEST(src_best->flags, BGP_ROUTE_FLAG_IMPORT))
    {
        src_best = NULL;
    }

    /* 推导目标 NLRI */
    bgp_nlri_entry_t tgt_nlri;
    bgp_import_rib_derive_target_nlri(&src_head->nlri, &tgt_nlri);

    bgp_rib_t *tgt_rib = bgp_inst_rib_ensure_for_nlri(tgt_inst, &tgt_nlri);
    if (!tgt_rib)
    {
        return 0;
    }

    bgp_rthead_t *tgt_head = (bgp_rthead_t *)bgp_rib_lookup_head(tgt_rib, &tgt_nlri);

    if (!src_best || !BIT_TEST(src_best->flags, BGP_ROUTE_FLAG_VALID))
    {
        /* 源 best 缺失：撤销 tgt_head 下的 mirror（按 src->source 不可知，需扫描） */
        if (!tgt_head)
        {
            return 1;
        }
        for (GList *l = tgt_head->route_list; l;)
        {
            bgp_route_node_t *r = (bgp_route_node_t *)l->data;
            l = l->next;
            if (r && BIT_TEST(r->flags, BGP_ROUTE_FLAG_IMPORT_RIB))
            {
                bgp_import_rib_mirror_withdraw(tgt_inst, tgt_rib, &tgt_nlri, r);
            }
        }
        return 1;
    }

    /* 源 best 存在：ensure tgt_head 后交给 mirror_create 做"已存在则复用、否则新建" */
    if (!tgt_head)
    {
        tgt_head = bgp_rib_ensure_head(tgt_rib, &tgt_nlri);
        if (!tgt_head)
        {
            return 0;
        }
    }
    (void)bgp_import_rib_mirror_create(tgt_inst, tgt_rib, tgt_head, &tgt_nlri, (bgp_route_node_t *)src_best);
    return 1;
}

int bgp_import_rib_queue_process(bgp_instance_t *inst, int batch)
{
    if (!inst || !inst->import_rib_state || batch <= 0)
    {
        return 0;
    }
    bgp_import_rib_state_t *st = (bgp_import_rib_state_t *)inst->import_rib_state;
    if (!st->pending)
    {
        return 0;
    }

    int processed = 0;
    bgp_rthead_t *head = NULL;
    while (processed < batch && (head = (bgp_rthead_t *)g_queue_pop_head(st->pending)) != NULL)
    {
        st->pending_count--;
        (void)bgp_import_rib_process_one(inst, head);
        /* 收尾 GC：head 来自源 inst（labeled），unref 后若已无任何队列引用且 route_list 空，
         * 必须从源 RIB tree 摘除，否则源 ensure_head 会复用残留空 head 导致 head->nlri 陈旧
         * （例如 labeled label 字段保留旧值），使后续 announce 携带过期 label。 */
        bgp_instance_t *src_inst = head->inst;
        bgp_rib_t *src_rib = src_inst ? bgp_inst_rib_for_nlri(src_inst, &head->nlri) : NULL;
        bgp_rib_head_unref(head);
        if (src_rib)
        {
            (void)bgp_rib_gc_head(src_rib, head);
        }
        processed++;
    }
    return processed;
}

void bgp_import_rib_drain_after_calc(bgp_instance_t *src_inst)
{
    bgp_instance_t *tgt_inst = bgp_import_rib_find_target_inst(src_inst);
    if (!tgt_inst)
    {
        return;
    }
    /* 一次最多处理 256 条，剩余的下次源 calc 事件再驱动；
     * 若 pending 还有剩余，单 NLRI 已 push 到目标 calc_queue，那一侧自有调度。 */
    (void)bgp_import_rib_queue_process(tgt_inst, 256);
}

int bgp_import_rib_process_pending(bgp_instance_t *inst)
{
    if (!inst || !inst->import_rib_state)
    {
        return 0;
    }
    bgp_import_rib_state_t *st = (bgp_import_rib_state_t *)inst->import_rib_state;
    int total = 0;
    while (st->pending_count > 0)
    {
        int n = bgp_import_rib_queue_process(inst, 256);
        if (n <= 0)
        {
            break;
        }
        total += n;
    }
    return total;
}

/* ============================================================================
 * enable / disable（CLI 入口）
 * ========================================================================== */

/**
 * @brief 遍历 src_inst 所有 RIB 的所有 head，push 到目标 inst 的 pending 队列（用于 enable 全量灌入）
 */
typedef struct
{
    bgp_instance_t *tgt_inst;
} enum_push_ctx_t;

static gboolean enum_push_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    enum_push_ctx_t *c = (enum_push_ctx_t *)user_data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;
    if (!c || !c->tgt_inst || !head || !head->inst)
    {
        return FALSE;
    }
    bgp_import_rib_state_t *st = (bgp_import_rib_state_t *)c->tgt_inst->import_rib_state;
    if (!st)
    {
        return FALSE;
    }
    bgp_rib_head_ref(head);
    g_queue_push_tail(st->pending, head);
    st->pending_count++;
    return FALSE;
}

static void enum_push_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer user_data)
{
    (void)inst;
    (void)entry;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    g_tree_foreach(rib->head_tree, enum_push_head_cb, user_data);
}

int bgp_import_rib_enable(bgp_instance_t *inst, bgp_import_src_t src)
{
    if (!inst || !inst->import_rib_state)
    {
        return -1;
    }
    /* 第一阶段仅支持 labeled-unicast */
    if (src != BGP_IMPORT_SRC_LABELED_UC)
    {
        LOG_WARN("BGP import-rib: source %u not supported yet", (unsigned)src);
        return -1;
    }
    if ((inst->import_rib_sources & (1U << src)) != 0U)
    {
        return 0;
    }
    inst->import_rib_sources |= (1U << src);

    /* 全量灌入：扫源 inst 所有 head，push 到本 inst 的 pending */
    if (inst->vrf && inst->safi == BGP_SAFI_UNICAST)
    {
        bgp_instance_t *src_inst =
            (bgp_instance_t *)g_hash_table_lookup(inst->vrf->inst_hash, bgp_inst_hash_key(inst->afi, BGP_SAFI_LABELED));
        if (src_inst)
        {
            enum_push_ctx_t c = {.tgt_inst = inst};
            bgp_inst_foreach_rib(src_inst, enum_push_rib_cb, &c);
        }
    }
    /* enable 是 CLI 触发的同步动作，没有外部 event 来驱动 pending 处理。
     * 直接在当前 worker 线程同步抽干一次，把 mirror 灌进 unicast RIB
     * 并推到 unicast calc_queue，由 calc 事件继续后续优选/下刷。 */
    (void)bgp_import_rib_process_pending(inst);

    LOG_INFO("BGP import-rib enabled: vrf=%u afi=%u safi=%u src=%u", inst->vrf ? inst->vrf->vrf_id : 0,
             (unsigned)inst->afi, (unsigned)inst->safi, (unsigned)src);
    return 0;
}

int bgp_import_rib_disable(bgp_instance_t *inst, bgp_import_src_t src)
{
    if (!inst || !inst->import_rib_state)
    {
        return -1;
    }
    if ((inst->import_rib_sources & (1U << src)) == 0U)
    {
        return 0;
    }
    inst->import_rib_sources &= ~(1U << src);

    bgp_import_rib_state_t *st = (bgp_import_rib_state_t *)inst->import_rib_state;

    /* 抽干 pending 队列：disable 前可能仍有 push 未处理；不应在 disable 后再消费它们重建 mirror */
    if (st->pending)
    {
        bgp_rthead_t *h = NULL;
        while ((h = (bgp_rthead_t *)g_queue_pop_head(st->pending)) != NULL)
        {
            bgp_rib_head_unref(h);
            if (st->pending_count > 0)
            {
                st->pending_count--;
            }
        }
    }

    /* 撤销所有 mirror：复制一份 src 指针快照，逐个撤 */
    GList *src_snapshot = NULL;
    if (st->mirror_by_src)
    {
        GHashTableIter iter;
        gpointer src_p = NULL;
        gpointer mirror_p = NULL;
        g_hash_table_iter_init(&iter, st->mirror_by_src);
        while (g_hash_table_iter_next(&iter, &src_p, &mirror_p))
        {
            src_snapshot = g_list_prepend(src_snapshot, src_p);
        }
    }

    for (GList *l = src_snapshot; l; l = l->next)
    {
        bgp_route_node_t *src = (bgp_route_node_t *)l->data;
        bgp_route_node_t *mirror = (bgp_route_node_t *)g_hash_table_lookup(st->mirror_by_src, src);
        if (!mirror || !mirror->head)
        {
            continue;
        }
        bgp_nlri_entry_t tgt_nlri = mirror->head->nlri;
        bgp_rib_t *tgt_rib = bgp_inst_rib_for_nlri(inst, &tgt_nlri);
        if (tgt_rib)
        {
            bgp_import_rib_mirror_withdraw(inst, tgt_rib, &tgt_nlri, mirror);
        }
    }
    g_list_free(src_snapshot);

    LOG_INFO("BGP import-rib disabled: vrf=%u afi=%u safi=%u src=%u", inst->vrf ? inst->vrf->vrf_id : 0,
             (unsigned)inst->afi, (unsigned)inst->safi, (unsigned)src);
    return 0;
}

/* ============================================================================
 * cfg_apply orchestrator
 * ========================================================================== */

void bgp_cfg_apply_import_rib(bgp_apply_cmd_t *apply)
{
    if (!apply)
    {
        return;
    }
    apply->rc = BGP_APPLY_RC_FAIL;

    if (!g_bgp_work_local || !g_bgp_work_local->protocol)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_protocol_t *proto = g_bgp_work_local->protocol;
    uint32_t vrf_id = BGP_VRF_PUBLIC_ID;
    if (apply->vrf_name[0] == '\0')
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Missing VRF name.");
        return;
    }
    if (strcmp(apply->vrf_name, VRF_PUBLIC_VRF_NAME) != 0)
    {
        const vrf_api_cache_entry_t *entry = vrf_api_cache_lookup_by_name(apply->vrf_name);
        if (!entry)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
            return;
        }
        vrf_id = entry->vrf_id;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    /* import-rib 仅允许在 unicast AF 视图下配置 */
    if (apply->u.import_rib.safi != BGP_SAFI_UNICAST)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 "BGP Error: import-rib only supported in unicast address-family.");
        return;
    }

    bgp_instance_t *inst = bgp_vrf_get_or_create_instance(vrf, apply->u.import_rib.afi, apply->u.import_rib.safi);
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Address family instance creation failed.");
        return;
    }

    uint32_t src_bit = 1U << apply->u.import_rib.src;
    uint32_t prev = inst->import_rib_sources;
    if (apply->isNo)
    {
        if ((prev & src_bit) == 0U)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            apply->out.import_rib_sources = prev;
            return;
        }
        (void)bgp_import_rib_disable(inst, (bgp_import_src_t)apply->u.import_rib.src);
    }
    else
    {
        if ((prev & src_bit) != 0U)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            apply->out.import_rib_sources = prev;
            return;
        }
        (void)bgp_import_rib_enable(inst, (bgp_import_src_t)apply->u.import_rib.src);
    }
    apply->out.import_rib_sources = inst->import_rib_sources;
    apply->rc = BGP_APPLY_RC_OK;
}
