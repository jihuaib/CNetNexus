/**
 * @file   bgp_vrf_export.c
 * @brief  VRF 路由导出到 vpnv4 实现：全量扫描 + 分批 + per-VRF 单标签 + 合 export RT
 * @author jhb
 * @date   2026/05/31
 */
#include "bgp_vrf_export.h"

#include <netinet/in.h>
#include <string.h>

#include "bgp.h"
#include "bgp_attr_intern.h"
#include "bgp_calc.h"
#include "bgp_ext_community.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_nexthop.h"
#include "bgp_protocol.h"
#include "bgp_rd.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "bit.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "tunnel.h"
#include "vrf.h"

/** 发送时 per-vrf 标签申请 RPC 超时 */
#define BGP_VRF_EXPORT_LABEL_TIMEOUT_MS 3000u

/** g_tree_foreach 回调：收集 head 指针到 GList(避免遍历期修改 tree)；定义见文件后段 */
static gboolean bgp_vrf_export_collect_head_cb(gpointer key, gpointer value, gpointer user_data);

static bgp_protocol_t *bgp_vrf_export_proto(void)
{
    return g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
}

/**
 * @brief 为私网 VRF 派生导出路由的合成来源地址(每 VRF 唯一，标识本地再生成 VPN 路由)
 *
 * 用 IPv4 地址承载 vrf_id：family=AF_INET，地址 = htonl(vrf_id)。该地址只作 RIB 键，
 * 与远端 vpnv4 邻居学到的(以邻居 IP 为来源)路由区分，保证同 (rd, prefix) 下本地导出
 * 与远端学习并存、各自被优选评估。
 */
static void bgp_vrf_export_synth_source(uint32_t vrf_id, net_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    out->family = AF_INET;
    out->u.v4.s_addr = htonl(vrf_id);
}

/**
 * @brief 发送时按 per-VRF 申请 VPN 聚合标签(向 TUNNEL 同步 RPC，结果缓存到 bgp_vrf)
 *
 * 语义(按需求):loc-rib 不带标签;首次发送某 VRF 的导出路由时才申请,申请到缓存复用;
 * 申请不到返回 0,调用方据此 hold(不发送),待标签可得后下次 pub 再发。
 * per-vrf 聚合标签:fec.prefix_len=0(整 VRF 一个标签),source_type=BGP_ADJ。
 *
 * @param vrf 源私网 VRF
 * @return 标签值(>0),0=暂不可得
 */
static uint32_t bgp_vrf_export_alloc_vrf_label(bgp_vrf_t *vrf)
{
    if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return 0;
    }
    if (vrf->vpn_label != 0u)
    {
        return vrf->vpn_label; /* 已申请，复用 */
    }
    if (!g_bgp_local || !g_bgp_local->dev_ipc_ctx)
    {
        return 0;
    }

    tunnel_label_req_t req;
    memset(&req, 0, sizeof(req));
    req.vrf_id = vrf->vrf_id;
    req.afi = (uint16_t)BGP_AFI_IPV4;
    req.source_type = TUNNEL_SOURCE_BGP_ADJ;
    req.owner_module_id = DEV_MODULE_ID_BGP;
    req.owner_id = ((uint32_t)BGP_AFI_IPV4 << 16) | (uint32_t)BGP_SAFI_VPN_UNICAST;
    req.fec.vrf_id = vrf->vrf_id;
    req.fec.afi = (uint16_t)BGP_AFI_IPV4;
    req.fec.prefix_len = 0;        /* 整 VRF 聚合，无具体前缀 */
    req.fec.addr.family = AF_INET; /* 占位 0.0.0.0：TUNNEL 要求 fec.addr.family 非 0 */

    uint32_t label = 0;
    if (tunnel_rpc_label_alloc(g_bgp_local->dev_ipc_ctx, &req, &label, BGP_VRF_EXPORT_LABEL_TIMEOUT_MS) !=
            ERRCODE_SUCCESS ||
        label == 0)
    {
        LOG_WARN("BGP vrf-export: per-vrf label alloc pending for VRF %u (TUNNEL unavailable), hold advertise",
                 vrf->vrf_id);
        return 0;
    }
    vrf->vpn_label = label;
    LOG_INFO("BGP vrf-export: allocated per-vrf VPN label %u for VRF %u", label, vrf->vrf_id);
    return label;
}

uint32_t bgp_vrf_export_resolve_send_label(const bgp_route_node_t *best)
{
    if (!best || !best->src_route || !best->src_route->head || !best->src_route->head->inst ||
        !best->src_route->head->inst->vrf)
    {
        return 0;
    }
    bgp_vrf_t *src_vrf = best->src_route->head->inst->vrf;

    /* 读 apply-label 模式(默认 per-vrf);per-route 本轮未实现，退化为 per-vrf */
    const vrf_api_af_t *af = vrf_api_cache_get_af(src_vrf->vrf_id, VRF_AFI_IPV4, VRF_SAFI_UNICAST);
    if (af && af->apply_label_mode == VRF_APPLY_LABEL_PER_ROUTE)
    {
        /* TODO(P-next): per-route 每前缀一标签;当前按 per-vrf 聚合处理 */
    }
    return bgp_vrf_export_alloc_vrf_label(src_vrf);
}

void bgp_vrf_export_release_vrf_label(bgp_vrf_t *vrf)
{
    if (!vrf || vrf->vpn_label == 0u || !g_bgp_local || !g_bgp_local->dev_ipc_ctx)
    {
        return;
    }
    tunnel_label_req_t req;
    memset(&req, 0, sizeof(req));
    req.vrf_id = vrf->vrf_id;
    req.afi = (uint16_t)BGP_AFI_IPV4;
    req.source_type = TUNNEL_SOURCE_BGP_ADJ;
    req.owner_module_id = DEV_MODULE_ID_BGP;
    req.owner_id = ((uint32_t)BGP_AFI_IPV4 << 16) | (uint32_t)BGP_SAFI_VPN_UNICAST;
    req.fec.vrf_id = vrf->vrf_id;
    req.fec.afi = (uint16_t)BGP_AFI_IPV4;
    req.fec.prefix_len = 0;
    req.fec.addr.family = AF_INET; /* 与 alloc 时一致，确保 release 命中同一 binding */
    (void)tunnel_rpc_label_release(g_bgp_local->dev_ipc_ctx, &req);
    vrf->vpn_label = 0u;
}

void bgp_vrf_export_inst_init(bgp_instance_t *inst)
{
    if (!inst || inst->vrf_export_state)
    {
        return;
    }
    /* 仅 public VRF 的 ipv4 vpn-unicast instance 承载导出状态 */
    if (!inst->vrf || inst->vrf->vrf_id != BGP_VRF_PUBLIC_ID || inst->afi != BGP_AFI_IPV4 ||
        inst->safi != BGP_SAFI_VPN_UNICAST)
    {
        return;
    }
    bgp_vrf_export_state_t *st = g_new0(bgp_vrf_export_state_t, 1);
    st->pending = g_queue_new();
    st->pending_count = 0;
    inst->vrf_export_state = st;
}

/**
 * @brief 解除导出节点对来源节点的 borrow 引用并清空 src_route
 *
 * 唯一负责释放 export->src_route borrow 的入口；所有撤销/拆除路径都经此。
 */
static void bgp_vrf_export_detach_src(bgp_route_node_t *exp_route)
{
    if (!exp_route || !exp_route->src_route)
    {
        return;
    }
    bgp_route_node_t *old_src = exp_route->src_route;
    exp_route->src_route = NULL;
    bgp_route_node_borrow_unref(old_src);
}

/** RIB 扫描回调：对一个 RIB 内所有导出(IMPORT)节点解除来源 borrow(inst_destroy 用，不撤销) */
static void bgp_vrf_export_detach_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer ud)
{
    (void)inst;
    (void)entry;
    (void)ud;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, bgp_vrf_export_collect_head_cb, &heads);
    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        if (!head)
        {
            continue;
        }
        for (GList *r = head->route_list; r; r = r->next)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)r->data;
            if (route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS))
            {
                bgp_vrf_export_detach_src(route);
            }
        }
    }
    g_list_free(heads);
}

void bgp_vrf_export_inst_destroy(bgp_instance_t *inst)
{
    if (!inst || !inst->vrf_export_state)
    {
        return;
    }
    bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)inst->vrf_export_state;
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
    /* 扫 vpnv4 RIB 内所有导出节点，解除对来源的 borrow(此刻本 inst 的 RIB 尚未销毁) */
    bgp_inst_foreach_rib(inst, bgp_vrf_export_detach_rib_cb, NULL);
    g_free(st);
    inst->vrf_export_state = NULL;
}

bgp_instance_t *bgp_vrf_export_target_inst(void)
{
    bgp_protocol_t *proto = bgp_vrf_export_proto();
    if (!proto)
    {
        return NULL;
    }
    bgp_vrf_t *pub = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
    if (!pub || !pub->inst_hash)
    {
        return NULL;
    }
    bgp_instance_t *inst =
        (bgp_instance_t *)g_hash_table_lookup(pub->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV4, BGP_SAFI_VPN_UNICAST));
    return (inst && inst->vrf_export_state) ? inst : NULL;
}

/* ============================================================================
 * 单条处理
 * ========================================================================== */

/**
 * @brief 由源 unicast NLRI + VRF RD 推导目标 vpnv4 NLRI（loc-rib 不带标签，标签发送时申请）
 */
static void bgp_vrf_export_derive_vpn_nlri(const bgp_nlri_entry_t *src, const bgp_rd_t *rd, bgp_nlri_entry_t *dst)
{
    *dst = *src;
    dst->safi = BGP_SAFI_VPN_UNICAST;
    if (dst->type == BGP_NLRI_PREFIX)
    {
        memcpy(dst->prefix.rd.bytes, rd->bytes, sizeof(dst->prefix.rd.bytes));
        dst->prefix.has_rd = true;
        dst->prefix.label = 0;
        dst->prefix.has_label = false;
    }
}

/**
 * @brief 处理一个源 unicast head：把当前 best reconcile 到 vpnv4 对应 RD 的 RIB
 */
static int bgp_vrf_export_process_one(bgp_instance_t *vpn_inst, bgp_rthead_t *src_head)
{
    if (!vpn_inst || !src_head)
    {
        return 0;
    }
    bgp_instance_t *src_inst = src_head->inst;
    if (!src_inst || !src_inst->vrf || src_inst->safi != BGP_SAFI_UNICAST || src_inst->afi != BGP_AFI_IPV4)
    {
        return 0;
    }
    uint32_t vrf_id = src_inst->vrf->vrf_id;
    if (vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return 0; /* 公网路由不进 vpnv4 */
    }

    /* 取该 VRF 的 RD(无 RD 不能进 VPN 表) */
    const vrf_api_af_t *af = vrf_api_cache_get_af(vrf_id, VRF_AFI_IPV4, VRF_SAFI_UNICAST);
    if (!af || !af->has_rd)
    {
        char nbuf[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(&src_head->nlri, nbuf, sizeof(nbuf));
        LOG_WARN("BGP vrf-export: VRF %u has no RD, skip %s", vrf_id, nbuf);
        return 0;
    }

    bgp_rd_t rd;
    memcpy(rd.bytes, af->rd.bytes, sizeof(rd.bytes));

    bgp_nlri_entry_t vpn_nlri;
    bgp_vrf_export_derive_vpn_nlri(&src_head->nlri, &rd, &vpn_nlri);

    bgp_rib_t *tgt_rib = bgp_inst_rib_ensure_for_nlri(vpn_inst, &vpn_nlri);
    if (!tgt_rib)
    {
        return 0;
    }

    net_addr_t synth;
    bgp_vrf_export_synth_source(vrf_id, &synth);

    /* 源 best(仅 VALID) */
    bgp_rib_t *src_rib = bgp_inst_rib_for_nlri(src_inst, &src_head->nlri);
    bgp_route_node_t *src_best = src_rib ? (bgp_route_node_t *)bgp_rib_find_best(src_rib, &src_head->nlri) : NULL;

    bgp_rthead_t *tgt_head = (bgp_rthead_t *)bgp_rib_lookup_head(tgt_rib, &vpn_nlri);
    bgp_route_node_t *route = tgt_head ? bgp_rthead_lookup_route_mut(tgt_head, &synth) : NULL;

    /* REMOTE_CROSS：该 best 是从 peer 的 vpnv4 导入到本 VRF 的路由，绝不能再导出回 vpnv4
     * （否则 r1→r2 导入后 r2 又用自己 RD 导出回去，撤销时续命成环）。等同 best 缺失：撤销本地导出。 */
    if (!src_best || !BIT_TEST(src_best->flags, BGP_ROUTE_FLAG_VALID) ||
        BIT_TEST(src_best->flags, BGP_ROUTE_FLAG_REMOTE_CROSS))
    {
        /* best 缺失/不可导出：解除溯源关联并撤销 vpnv4 中该 (rd, prefix) 的本地导出路由 */
        if (route)
        {
            bgp_vrf_export_detach_src(route);
        }
        if (bgp_rib_unreach_one(tgt_rib, &vpn_nlri, &synth) == 1 && vpn_inst->calc_queue)
        {
            bgp_calc_queue_push(vpn_inst->calc_queue, vpn_inst, &vpn_nlri);
        }
        return 1;
    }

    /* best 存在：拷贝属性 + 合 export RT，作为本地导出路径写入 vpnv4 RIB */
    bgp_attr_t attr = *BGP_ROUTE_ATTR(src_best);
    bgp_ext_community_merge_vrf_export_rts(&attr, vrf_id, BGP_AFI_IPV4);

    if (!tgt_head)
    {
        tgt_head = bgp_rib_ensure_head(tgt_rib, &vpn_nlri);
    }
    if (!tgt_head)
    {
        return 0;
    }
    if (!route)
    {
        route = bgp_rthead_lookup_route_mut(tgt_head, &synth);
    }
    if (!route)
    {
        route = bgp_rthead_create_route(tgt_rib, tgt_head, &synth);
        if (!route)
        {
            return 0;
        }
    }
    bgp_nexthop_reset_route(route);
    if (bgp_rib_route_apply_reach(route, ROUTE_PROTOCOL_BGP, &attr) != 0)
    {
        return 0;
    }
    /* 本地跨表合成路由：清 IMPORT、置 LOCAL_CROSS。它是本地起源（通告语义等同重分发），
     * 但不是 import-route，避免被 import-route 清理误删；也便于对端区分回灌成环。 */
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_IMPORT);
    BIT_SET(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS);
    /* loc-rib 不带标签：标签在 update_group 发送时按 per-vrf 申请并注入 NLRI */

    /* 维护溯源前向指针：best 切换来源时换 borrow 引用 */
    if (route->src_route != src_best)
    {
        bgp_vrf_export_detach_src(route);
        route->src_route = src_best;
        bgp_route_node_borrow_ref(src_best);
    }

    if (vpn_inst->calc_queue)
    {
        bgp_calc_queue_push(vpn_inst->calc_queue, vpn_inst, &vpn_nlri);
    }
    return 1;
}

/* ============================================================================
 * pending 队列 + 批处理
 * ========================================================================== */

static void bgp_vrf_export_push_head(bgp_vrf_export_state_t *st, bgp_rthead_t *head)
{
    if (!st || !st->pending || !head)
    {
        return;
    }
    bgp_rib_head_ref(head);
    g_queue_push_tail(st->pending, head);
    st->pending_count++;
}

int bgp_vrf_export_queue_process(bgp_instance_t *vpn_inst, int batch)
{
    if (!vpn_inst || !vpn_inst->vrf_export_state || batch <= 0)
    {
        return 0;
    }
    bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)vpn_inst->vrf_export_state;
    if (!st->pending)
    {
        return 0;
    }

    int processed = 0;
    bgp_rthead_t *head = NULL;
    while (processed < batch && (head = (bgp_rthead_t *)g_queue_pop_head(st->pending)) != NULL)
    {
        if (st->pending_count > 0)
        {
            st->pending_count--;
        }
        (void)bgp_vrf_export_process_one(vpn_inst, head);

        /* 收尾 GC：源 head 来自私网 unicast inst，unref 后若已无队列引用且为空则摘除 */
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

int bgp_vrf_export_process_pending(bgp_instance_t *vpn_inst)
{
    if (!vpn_inst || !vpn_inst->vrf_export_state)
    {
        return 0;
    }
    bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)vpn_inst->vrf_export_state;
    int total = 0;
    while (st->pending_count > 0)
    {
        int n = bgp_vrf_export_queue_process(vpn_inst, BGP_VRF_EXPORT_BATCH);
        if (n <= 0)
        {
            break;
        }
        total += n;
    }
    return total;
}

/* ============================================================================
 * 增量入口
 * ========================================================================== */

void bgp_vrf_export_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head)
{
    if (!src_inst || !head)
    {
        return;
    }
    if (src_inst->afi != BGP_AFI_IPV4 || src_inst->safi != BGP_SAFI_UNICAST)
    {
        return;
    }
    if (!src_inst->vrf || src_inst->vrf->vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return;
    }
    bgp_instance_t *vpn_inst = bgp_vrf_export_target_inst();
    if (!vpn_inst)
    {
        return; /* vpnv4 未使能 */
    }
    bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)vpn_inst->vrf_export_state;
    bgp_vrf_export_push_head(st, head);
    (void)bgp_worker_post_vrf_export_event();
}

/* ============================================================================
 * enable / disable
 * ========================================================================== */

/** enable 全量扫描：把一个 unicast RIB 的所有 head 推入 vpnv4 pending */
static gboolean bgp_vrf_export_scan_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)user_data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;
    if (head && head->inst)
    {
        bgp_vrf_export_push_head(st, head);
    }
    return FALSE;
}

static void bgp_vrf_export_scan_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer user_data)
{
    (void)inst;
    (void)entry;
    if (rib && rib->head_tree)
    {
        g_tree_foreach(rib->head_tree, bgp_vrf_export_scan_head_cb, user_data);
    }
}

int bgp_vrf_export_enable(bgp_instance_t *vpn_inst)
{
    if (!vpn_inst || !vpn_inst->vrf_export_state)
    {
        return -1;
    }
    bgp_protocol_t *proto = bgp_vrf_export_proto();
    if (!proto || !proto->vrf_hash)
    {
        return 0;
    }
    bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)vpn_inst->vrf_export_state;

    /* 遍历所有私网 VRF 的 ipv4-unicast inst，全部 head 入 pending */
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&iter, proto->vrf_hash);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)val;
        if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID || !vrf->inst_hash)
        {
            continue;
        }
        bgp_instance_t *uc_inst =
            (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV4, BGP_SAFI_UNICAST));
        if (uc_inst)
        {
            bgp_inst_foreach_rib(uc_inst, bgp_vrf_export_scan_rib_cb, st);
        }
    }

    LOG_INFO("BGP vrf-export enabled: queued %u source heads", st->pending_count);
    /* 不在此同步抽干：投事件由 worker 分批处理 */
    (void)bgp_worker_post_vrf_export_event();
    return 0;
}

void bgp_vrf_export_backfill_vrf(uint32_t vrf_id)
{
    if (vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return;
    }
    bgp_instance_t *vpn_inst = bgp_vrf_export_target_inst();
    if (!vpn_inst)
    {
        return; /* vpnv4 未使能，RD 配置稍后由 enable 全量扫描覆盖 */
    }
    bgp_protocol_t *proto = bgp_vrf_export_proto();
    if (!proto)
    {
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
    if (!vrf || !vrf->inst_hash)
    {
        return;
    }
    bgp_instance_t *uc_inst =
        (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV4, BGP_SAFI_UNICAST));
    if (!uc_inst)
    {
        return; /* 该 VRF 还没有 ipv4-unicast 路由 */
    }

    bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)vpn_inst->vrf_export_state;
    uint32_t before = st->pending_count;
    bgp_inst_foreach_rib(uc_inst, bgp_vrf_export_scan_rib_cb, st);
    if (st->pending_count > before)
    {
        LOG_INFO("BGP vrf-export: backfill VRF %u queued %u source heads (RD configured)", vrf_id,
                 st->pending_count - before);
        (void)bgp_worker_post_vrf_export_event();
    }
}

/** g_tree_foreach 回调：收集 head 指针到 GList(避免遍历期修改 tree) */
static gboolean bgp_vrf_export_collect_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    GList **pp = (GList **)user_data;
    *pp = g_list_prepend(*pp, value);
    return FALSE;
}

/** disable：撤销 vpnv4 某 RIB 中所有本地导出(IMPORT 标记)路由 */
static void bgp_vrf_export_purge_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer user_data)
{
    (void)entry;
    (void)user_data;
    if (!rib || !rib->head_tree)
    {
        return;
    }

    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, bgp_vrf_export_collect_head_cb, &heads);

    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        if (!head)
        {
            continue;
        }
        for (GList *r = head->route_list; r;)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)r->data;
            r = r->next;
            if (route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS))
            {
                net_addr_t src = route->source;
                bgp_vrf_export_detach_src(route);
                if (bgp_rib_unreach_one(rib, &head->nlri, &src) == 1 && inst->calc_queue)
                {
                    bgp_calc_queue_push(inst->calc_queue, inst, &head->nlri);
                }
            }
        }
    }
    g_list_free(heads);
}

int bgp_vrf_export_disable(bgp_instance_t *vpn_inst)
{
    if (!vpn_inst || !vpn_inst->vrf_export_state)
    {
        return -1;
    }
    bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)vpn_inst->vrf_export_state;

    /* 1. 清空 pending(disable 后不应再消费重建) */
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

    /* 2. 撤销所有已导出 VPN 路由(同时解除溯源 borrow) */
    bgp_inst_foreach_rib(vpn_inst, bgp_vrf_export_purge_rib_cb, st);
    LOG_INFO("BGP vrf-export disabled");
    return 0;
}

/** purge_source_inst 扫描上下文 */
typedef struct
{
    bgp_instance_t *src_inst; /**< 目标来源 instance */
    GPtrArray *victims;       /**< 收集到的导出节点(其 src_route 来自 src_inst) */
} bgp_vrf_export_purge_ctx_t;

/** 扫 vpn RIB 收集 src_route 属于 src_inst 的导出节点 */
static void bgp_vrf_export_collect_victims_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer ud)
{
    (void)inst;
    (void)entry;
    bgp_vrf_export_purge_ctx_t *ctx = (bgp_vrf_export_purge_ctx_t *)ud;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, bgp_vrf_export_collect_head_cb, &heads);
    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        if (!head)
        {
            continue;
        }
        for (GList *r = head->route_list; r; r = r->next)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)r->data;
            /* src_route->head 此刻仍存活(来源 inst 尚未销毁 RIB) */
            if (route && route->src_route && route->src_route->head && route->src_route->head->inst == ctx->src_inst)
            {
                g_ptr_array_add(ctx->victims, route);
            }
        }
    }
    g_list_free(heads);
}

void bgp_vrf_export_purge_source_inst(bgp_instance_t *src_inst)
{
    if (!src_inst || !src_inst->vrf || src_inst->afi != BGP_AFI_IPV4 || src_inst->safi != BGP_SAFI_UNICAST)
    {
        return;
    }
    if (src_inst->vrf->vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return;
    }
    bgp_instance_t *vpn_inst = bgp_vrf_export_target_inst();
    if (!vpn_inst)
    {
        return; /* vpnv4 未使能，无导出 */
    }

    /* 扫 vpn RIB 收集源属于 src_inst 的导出节点，逐个撤销(此刻源 RIB 仍存活) */
    GPtrArray *victims = g_ptr_array_new();
    bgp_vrf_export_purge_ctx_t ctx = {.src_inst = src_inst, .victims = victims};
    bgp_inst_foreach_rib(vpn_inst, bgp_vrf_export_collect_victims_cb, &ctx);

    for (guint i = 0; i < victims->len; i++)
    {
        bgp_route_node_t *exp_route = (bgp_route_node_t *)g_ptr_array_index(victims, i);
        if (!exp_route || !exp_route->head)
        {
            continue;
        }
        bgp_rib_t *tgt_rib =
            exp_route->head->inst ? bgp_inst_rib_for_nlri(exp_route->head->inst, &exp_route->head->nlri) : NULL;
        net_addr_t synth = exp_route->source;
        bgp_nlri_entry_t nlri = exp_route->head->nlri;
        bgp_vrf_export_detach_src(exp_route);
        if (tgt_rib && bgp_rib_unreach_one(tgt_rib, &nlri, &synth) == 1 && vpn_inst->calc_queue)
        {
            bgp_calc_queue_push(vpn_inst->calc_queue, vpn_inst, &nlri);
        }
    }
    if (victims->len > 0)
    {
        LOG_INFO("BGP vrf-export: purged %u routes sourced from VRF %u (inst teardown)", victims->len,
                 src_inst->vrf->vrf_id);
    }
    g_ptr_array_free(victims, TRUE);
}
