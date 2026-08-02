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
#define BGP_VRF_EXPORT_SID_TIMEOUT_MS 5000u
/** SID RPC 失败使用 1,2,4,8,16,30s 有界指数退避，成功抽干后清零。 */
#define BGP_VRF_EXPORT_RETRY_BASE_USEC ((gint64)G_USEC_PER_SEC)
#define BGP_VRF_EXPORT_RETRY_MAX_USEC (30 * (gint64)G_USEC_PER_SEC)
#define BGP_VRF_EXPORT_RETRY_MAX_EXP 5u

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
    const vrf_api_af_t *af = vrf_api_cache_get_af(src_vrf->vrf_id, VRF_AFI_IPV4);
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

static srv6_sid_entry_t *bgp_vrf_export_sid_slot(bgp_vrf_t *vrf, bgp_afi_t afi)
{
    if (!vrf)
    {
        return NULL;
    }
    return (afi == BGP_AFI_IPV4) ? &vrf->srv6_sid_v4 : (afi == BGP_AFI_IPV6) ? &vrf->srv6_sid_v6 : NULL;
}

/** 从私网 unicast AF 配置重建 BGP 的稳定 SID key。BGP 重启后本地 cache 为空，
 * 但 SRV6 会从自己的 DB 恢复 binding；删除配置不能因 cache 为空而跳过释放。 */
static gboolean bgp_vrf_export_build_sid_key(const bgp_vrf_t *vrf, bgp_afi_t afi, srv6_sid_key_t *key)
{
    bgp_instance_t *src_inst = (vrf && vrf->inst_hash && (afi == BGP_AFI_IPV4 || afi == BGP_AFI_IPV6))
                                   ? g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, BGP_SAFI_UNICAST))
                                   : NULL;
    if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID || !key || !src_inst || src_inst->srv6_locator[0] == '\0')
    {
        return FALSE;
    }

    memset(key, 0, sizeof(*key));
    g_strlcpy(key->locator, src_inst->srv6_locator, sizeof(key->locator));
    key->vrf_id = vrf->vrf_id;
    key->behavior = (afi == BGP_AFI_IPV4) ? SRV6_BEHAVIOR_END_DT4 : SRV6_BEHAVIOR_END_DT6;
    key->owner_module_id = DEV_MODULE_ID_BGP;
    key->owner_id = ((uint32_t)afi << 16) | (uint32_t)BGP_SAFI_UNICAST;
    return TRUE;
}

int bgp_vrf_export_release_srv6_sid(bgp_vrf_t *vrf, bgp_afi_t afi)
{
    srv6_sid_entry_t *entry = bgp_vrf_export_sid_slot(vrf, afi);
    if (!entry)
    {
        return ERRCODE_SUCCESS;
    }

    if (entry->key.locator[0] == '\0')
    {
        srv6_sid_key_t recovered_key;
        if (!bgp_vrf_export_build_sid_key(vrf, afi, &recovered_key))
        {
            return ERRCODE_SUCCESS;
        }
        entry->key = recovered_key;
    }
    if (!g_bgp_local || !g_bgp_local->dev_ipc_ctx ||
        srv6_rpc_sid_release(g_bgp_local->dev_ipc_ctx, &entry->key, BGP_VRF_EXPORT_SID_TIMEOUT_MS) != ERRCODE_SUCCESS)
    {
        /* RPC 超时存在“服务端已释放、响应丢失”的不确定态。保留稳定 key
         * 以便下一次 release/alloc 幂等确认，但清掉 SID，禁止直接复用并广告。 */
        srv6_sid_key_t uncertain_key = entry->key;
        memset(entry, 0, sizeof(*entry));
        entry->key = uncertain_key;
        LOG_WARN("BGP vrf-export: failed to release service SID vrf=%u afi=%u", vrf ? vrf->vrf_id : 0u, (unsigned)afi);
        return ERRCODE_FAIL;
    }
    memset(entry, 0, sizeof(*entry));
    return ERRCODE_SUCCESS;
}

int bgp_vrf_export_reconcile_srv6_sid_absent(bgp_vrf_t *vrf, bgp_afi_t afi)
{
    srv6_sid_entry_t *slot = bgp_vrf_export_sid_slot(vrf, afi);
    if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID || !slot || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6) ||
        !g_bgp_local || !g_bgp_local->dev_ipc_ctx)
    {
        return ERRCODE_FAIL;
    }

    srv6_sid_owner_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.vrf_id = vrf->vrf_id;
    scope.behavior = (afi == BGP_AFI_IPV4) ? SRV6_BEHAVIOR_END_DT4 : SRV6_BEHAVIOR_END_DT6;
    scope.owner_id = ((uint32_t)afi << 16) | (uint32_t)BGP_SAFI_UNICAST;
    if (srv6_rpc_sid_release_owner(g_bgp_local->dev_ipc_ctx, &scope, BGP_VRF_EXPORT_SID_TIMEOUT_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP vrf-export: owner-scope SID reconcile failed vrf=%u afi=%u", vrf->vrf_id, (unsigned)afi);
        return ERRCODE_FAIL;
    }
    memset(slot, 0, sizeof(*slot));
    return ERRCODE_SUCCESS;
}

void bgp_vrf_export_release_srv6_sids(bgp_vrf_t *vrf)
{
    (void)bgp_vrf_export_release_srv6_sid(vrf, BGP_AFI_IPV4);
    (void)bgp_vrf_export_release_srv6_sid(vrf, BGP_AFI_IPV6);
}

static const srv6_sid_entry_t *bgp_vrf_export_alloc_service_sid(bgp_instance_t *src_inst)
{
    bgp_vrf_t *src_vrf = src_inst ? src_inst->vrf : NULL;
    if (!src_inst || !src_vrf || src_vrf->vrf_id == BGP_VRF_PUBLIC_ID ||
        (src_inst->afi != BGP_AFI_IPV4 && src_inst->afi != BGP_AFI_IPV6) || src_inst->safi != BGP_SAFI_UNICAST ||
        src_inst->srv6_locator[0] == '\0' || !g_bgp_local || !g_bgp_local->dev_ipc_ctx)
    {
        return NULL;
    }
    srv6_sid_entry_t *slot = bgp_vrf_export_sid_slot(src_vrf, src_inst->afi);
    if (!slot)
    {
        return NULL;
    }

    srv6_sid_key_t key;
    if (!bgp_vrf_export_build_sid_key(src_vrf, src_inst->afi, &key))
    {
        return NULL;
    }

    const gboolean same_key = slot->key.locator[0] != '\0' && strcmp(slot->key.locator, key.locator) == 0 &&
                              slot->key.vrf_id == key.vrf_id && slot->key.behavior == key.behavior &&
                              slot->key.owner_module_id == key.owner_module_id && slot->key.owner_id == key.owner_id;
    if (same_key && slot->sid.family == AF_INET6)
    {
        return slot;
    }
    if (slot->key.locator[0] != '\0' && !same_key &&
        bgp_vrf_export_release_srv6_sid(src_vrf, src_inst->afi) != ERRCODE_SUCCESS)
    {
        return NULL;
    }

    srv6_sid_entry_t confirmed;
    memset(&confirmed, 0, sizeof(confirmed));
    if (srv6_rpc_sid_alloc(g_bgp_local->dev_ipc_ctx, &key, &confirmed, BGP_VRF_EXPORT_SID_TIMEOUT_MS) !=
            ERRCODE_SUCCESS ||
        confirmed.sid.family != AF_INET6)
    {
        /* alloc 同样可能只丢了响应；保存 key、不保存未经确认的 SID，下次
         * alloc 会以同一 key 查询/重装，而不会误当成可广告缓存。 */
        memset(slot, 0, sizeof(*slot));
        slot->key = key;
        LOG_WARN("BGP vrf-export: service SID pending vrf=%u afi=%u locator=%s", src_vrf->vrf_id,
                 (unsigned)src_inst->afi, src_inst->srv6_locator);
        return NULL;
    }
    *slot = confirmed;
    return slot;
}

int bgp_vrf_export_prepare_srv6_sid(bgp_instance_t *src_inst)
{
    if (!src_inst || !src_inst->vrf || src_inst->vrf->vrf_id == BGP_VRF_PUBLIC_ID ||
        (src_inst->afi != BGP_AFI_IPV4 && src_inst->afi != BGP_AFI_IPV6) || src_inst->safi != BGP_SAFI_UNICAST)
    {
        return ERRCODE_FAIL;
    }
    if (src_inst->srv6_locator[0] == '\0')
    {
        return ERRCODE_SUCCESS;
    }
    return bgp_vrf_export_alloc_service_sid(src_inst) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int bgp_vrf_export_apply_srv6_sid(const bgp_route_node_t *best, bgp_attr_t *out_attr)
{
    if (!best || !out_attr)
    {
        return ERRCODE_FAIL;
    }

    /* A public VPN route imported directly into that table has no private-AF
     * owner and therefore no End.DT service SID.  Keep its MPLS encoding. */
    if (!best->src_route || !best->src_route->head || !best->src_route->head->inst)
    {
        return ERRCODE_SUCCESS;
    }

    bgp_instance_t *src_inst = best->src_route->head->inst;
    bgp_vrf_t *src_vrf = src_inst->vrf;
    if (!src_vrf || src_vrf->vrf_id == BGP_VRF_PUBLIC_ID ||
        (src_inst->afi != BGP_AFI_IPV4 && src_inst->afi != BGP_AFI_IPV6) || src_inst->safi != BGP_SAFI_UNICAST)
    {
        return ERRCODE_SUCCESS;
    }

    /* `neighbor ... srv6-sid` selects the wire encoding.  A source AF with no
     * locator still uses the normal MPLS VPN label for that neighbor. */
    if (src_inst->srv6_locator[0] == '\0')
    {
        return ERRCODE_SUCCESS;
    }

    srv6_sid_key_t expected;
    srv6_sid_entry_t *slot = bgp_vrf_export_sid_slot(src_vrf, src_inst->afi);
    if (!slot || !bgp_vrf_export_build_sid_key(src_vrf, src_inst->afi, &expected) || slot->sid.family != AF_INET6 ||
        strcmp(slot->key.locator, expected.locator) != 0 || slot->key.vrf_id != expected.vrf_id ||
        slot->key.behavior != expected.behavior || slot->key.owner_module_id != expected.owner_module_id ||
        slot->key.owner_id != expected.owner_id)
    {
        /* Locator is configured, so silently falling back to MPLS would hide a
         * broken LocalSID transaction.  Hold this SID subgroup fail closed. */
        return ERRCODE_FAIL;
    }

    return bgp_attr_set_srv6_l3_service(out_attr, &slot->sid, slot->key.behavior, 0u, NULL) == 0 ? ERRCODE_SUCCESS
                                                                                                 : ERRCODE_FAIL;
}

void bgp_vrf_export_inst_init(bgp_instance_t *inst)
{
    if (!inst || inst->vrf_export_state)
    {
        return;
    }
    /* 仅 public VRF 的 VPN 类 instance 承载导出状态 */
    if (!inst->vrf || inst->vrf->vrf_id != BGP_VRF_PUBLIC_ID ||
        !(((inst->afi == BGP_AFI_IPV4 || inst->afi == BGP_AFI_IPV6) && inst->safi == BGP_SAFI_VPN_UNICAST) ||
          (inst->afi == BGP_AFI_L2VPN && inst->safi == BGP_SAFI_EVPN)))
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
    return bgp_vrf_export_target_inst_by_af(BGP_AFI_IPV4, BGP_SAFI_VPN_UNICAST);
}

bgp_instance_t *bgp_vrf_export_target_inst_by_af(bgp_afi_t afi, bgp_safi_t safi)
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
    bgp_instance_t *inst = (bgp_instance_t *)g_hash_table_lookup(pub->inst_hash, bgp_inst_hash_key(afi, safi));
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

static uint16_t evpn_type5_build_raw(const bgp_nlri_evpn_t *e, uint8_t raw[512])
{
    if (!e || !raw || e->ip_prefix.addr.family != AF_INET)
    {
        return 0;
    }

    uint8_t pfx_bytes = (uint8_t)((e->ip_prefix.prefix_len + 7u) / 8u);
    if (pfx_bytes > 4)
    {
        return 0;
    }

    const uint8_t vlen = (uint8_t)(30u + pfx_bytes); /* fixed(30) + variable IPv4 prefix bytes */
    uint16_t pos = 0;
    raw[pos++] = 5u;
    raw[pos++] = vlen;
    memcpy(raw + pos, e->rd.bytes, 8);
    pos += 8;
    memcpy(raw + pos, e->esi.bytes, 10);
    pos += 10;
    raw[pos++] = (uint8_t)(e->eth_tag >> 24);
    raw[pos++] = (uint8_t)(e->eth_tag >> 16);
    raw[pos++] = (uint8_t)(e->eth_tag >> 8);
    raw[pos++] = (uint8_t)e->eth_tag;
    raw[pos++] = (uint8_t)(pfx_bytes * 8u);
    if (pfx_bytes > 0)
    {
        memcpy(raw + pos, &e->ip_prefix.addr.u.v4, pfx_bytes);
        pos += pfx_bytes;
    }
    if (e->gw_ip.family == AF_INET)
    {
        memcpy(raw + pos, &e->gw_ip.u.v4, 4);
    }
    else
    {
        memset(raw + pos, 0, 4);
    }
    pos += 4;
    raw[pos++] = (uint8_t)(e->label1 >> 12);
    raw[pos++] = (uint8_t)(e->label1 >> 4);
    raw[pos++] = (uint8_t)(((e->label1 & 0xFu) << 4) | 0x01u);
    return pos;
}

static void bgp_vrf_export_derive_evpn_nlri(const bgp_nlri_entry_t *src, const bgp_rd_t *rd, bgp_nlri_entry_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->afi = BGP_AFI_L2VPN;
    dst->safi = BGP_SAFI_EVPN;
    dst->type = BGP_NLRI_EVPN;
    dst->evpn.route_type = 5;
    dst->evpn.rd = *rd;
    dst->evpn.eth_tag = 0;
    dst->evpn.ip_prefix = src->prefix.prefix;
    dst->evpn.ip_prefix.prefix_len = (uint8_t)(((dst->evpn.ip_prefix.prefix_len + 7u) / 8u) * 8u);
    dst->evpn.gw_ip.family = AF_INET;
    dst->evpn.label1 = 0;
    dst->evpn.raw_len = evpn_type5_build_raw(&dst->evpn, dst->evpn.raw);
}

static gboolean bgp_vrf_export_is_evpn_target(const bgp_instance_t *inst)
{
    return inst && inst->afi == BGP_AFI_L2VPN && inst->safi == BGP_SAFI_EVPN;
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
    gboolean evpn_target = bgp_vrf_export_is_evpn_target(vpn_inst);
    bgp_afi_t source_afi = evpn_target ? BGP_AFI_IPV4 : vpn_inst->afi;
    if (!src_inst || !src_inst->vrf || src_inst->safi != BGP_SAFI_UNICAST || src_inst->afi != source_afi)
    {
        return 0;
    }
    uint32_t vrf_id = src_inst->vrf->vrf_id;
    if (vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return 0; /* 公网路由不进 vpnv4 */
    }

    /* 取该 VRF 的 RD(无 RD 不能进 VPN 表) */
    const vrf_api_af_t *af = vrf_api_cache_get_af(vrf_id, (uint16_t)source_afi);
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
    if (evpn_target)
    {
        bgp_vrf_export_derive_evpn_nlri(&src_head->nlri, &rd, &vpn_nlri);
    }
    else
    {
        bgp_vrf_export_derive_vpn_nlri(&src_head->nlri, &rd, &vpn_nlri);
    }

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

    /* REMOTE_CROSS：从 peer 的 vpnv4 导入到本 VRF 的路由；LOCAL_CROSS：本机另一 VRF 泄漏进来的路由。
     * 二者都不能再导出回 vpnv4（REMOTE_CROSS 会与对端续命成环；LOCAL_CROSS 属本地交叉单跳，
     * 不应经 vpnv4 再传递）。等同 best 缺失：撤销本地导出。 */
    if (!src_best || !BIT_TEST(src_best->flags, BGP_ROUTE_FLAG_VALID) ||
        (evpn_target && !BIT_TEST(src_inst->flags, BGP_INST_FLAG_ADVERTISE_EVPN_ROUTE)) ||
        BIT_TEST(src_best->flags, BGP_ROUTE_FLAG_REMOTE_CROSS) || BIT_TEST(src_best->flags, BGP_ROUTE_FLAG_LOCAL_CROSS))
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
    if (evpn_target)
    {
        bgp_ext_community_merge_vrf_evpn_export_rts(&attr, vrf_id, BGP_AFI_IPV4);
    }
    else
    {
        bgp_ext_community_merge_vrf_export_rts(&attr, vrf_id, source_afi);
    }

    /* A private unicast route may itself carry Prefix-SID. A VPN export owns
     * its service attribute, so never leak that unrelated value. */
    bgp_attr_clear_prefix_sid(&attr);

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

static void bgp_vrf_export_retry_reset(bgp_vrf_export_state_t *st)
{
    if (!st)
    {
        return;
    }
    st->failed_count = 0u;
    st->retry_due_usec = 0;
    st->retry_backoff_exp = 0u;
    st->retry_event_armed = FALSE;
}

static void bgp_vrf_export_retry_schedule(bgp_instance_t *vpn_inst, bgp_vrf_export_state_t *st)
{
    if (!st)
    {
        return;
    }
    guint exp = MIN((guint)st->retry_backoff_exp, (guint)BGP_VRF_EXPORT_RETRY_MAX_EXP);
    gint64 delay = BGP_VRF_EXPORT_RETRY_BASE_USEC << exp;
    delay = MIN(delay, BGP_VRF_EXPORT_RETRY_MAX_USEC);
    st->retry_due_usec = g_get_monotonic_time() + delay;
    st->retry_event_armed = FALSE;
    if (st->retry_backoff_exp < BGP_VRF_EXPORT_RETRY_MAX_EXP)
    {
        st->retry_backoff_exp++;
    }
    LOG_WARN("BGP vrf-export: deferred retry afi=%u safi=%u in %" G_GINT64_FORMAT " ms (pending=%u)",
             vpn_inst ? (unsigned)vpn_inst->afi : 0u, vpn_inst ? (unsigned)vpn_inst->safi : 0u, delay / 1000,
             st->pending_count);
}

gboolean bgp_vrf_export_queue_ready(const bgp_instance_t *vpn_inst)
{
    const bgp_vrf_export_state_t *st = vpn_inst ? vpn_inst->vrf_export_state : NULL;
    if (!st || !st->pending || st->pending_count == 0u)
    {
        return FALSE;
    }
    return st->retry_due_usec == 0 || g_get_monotonic_time() >= st->retry_due_usec;
}

uint32_t bgp_vrf_export_pending_count(const bgp_instance_t *vpn_inst)
{
    const bgp_vrf_export_state_t *st = vpn_inst ? vpn_inst->vrf_export_state : NULL;
    return st ? st->pending_count : 0u;
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
    st->retry_event_armed = FALSE;
    if (st->retry_due_usec != 0)
    {
        if (g_get_monotonic_time() < st->retry_due_usec)
        {
            return 0;
        }
        st->retry_due_usec = 0;
    }

    int processed = 0;
    bgp_rthead_t *head = NULL;
    while (processed < batch && (head = (bgp_rthead_t *)g_queue_pop_head(st->pending)) != NULL)
    {
        if (st->pending_count > 0)
        {
            st->pending_count--;
        }
        int rc = bgp_vrf_export_process_one(vpn_inst, head);
        if (rc < 0)
        {
            st->failed_count++;
            /* 保留出队时已持有的 head ref，放回队尾后停止本批。
             * 失败通常表示 SRV6 模块级故障，继续消费只会放大 RPC 超时；
             * retry_due 到期后由 worker tick 重投，不在 eventfd 队列中自旋。 */
            g_queue_push_tail(st->pending, head);
            st->pending_count++;
            bgp_vrf_export_retry_schedule(vpn_inst, st);
            processed++;
            break;
        }
        /* 任意一条成功证明 SRV6 RPC 通路已恢复；后续若再失败
         * 应从 1s 重新起算，不继承故障期的高退避。failed_count 仍保留，
         * 供本次同步 drain 正确返回失败。 */
        st->retry_due_usec = 0;
        st->retry_backoff_exp = 0u;
        st->retry_event_armed = FALSE;

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
    if (st->pending_count == 0u)
    {
        bgp_vrf_export_retry_reset(st);
    }
    return processed;
}

void bgp_vrf_export_retry_tick(void)
{
    bgp_instance_t *targets[] = {
        bgp_vrf_export_target_inst_by_af(BGP_AFI_IPV4, BGP_SAFI_VPN_UNICAST),
        bgp_vrf_export_target_inst_by_af(BGP_AFI_IPV6, BGP_SAFI_VPN_UNICAST),
        bgp_vrf_export_target_inst_by_af(BGP_AFI_L2VPN, BGP_SAFI_EVPN),
    };
    gint64 now = g_get_monotonic_time();
    gboolean due = FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(targets); ++i)
    {
        bgp_vrf_export_state_t *st = targets[i] ? targets[i]->vrf_export_state : NULL;
        if (st && st->pending_count > 0u && st->retry_due_usec != 0 && now >= st->retry_due_usec &&
            !st->retry_event_armed)
        {
            due = TRUE;
        }
    }
    if (!due || bgp_worker_post_vrf_export_event() != 0)
    {
        return;
    }
    /* 一条全局 export 事件会消费所有 target；标记所有已到期队列，
     * 防止 epoll 下一轮在前一事件未处理时重复投递。 */
    for (size_t i = 0; i < G_N_ELEMENTS(targets); ++i)
    {
        bgp_vrf_export_state_t *st = targets[i] ? targets[i]->vrf_export_state : NULL;
        if (st && st->pending_count > 0u && st->retry_due_usec != 0 && now >= st->retry_due_usec)
        {
            st->retry_event_armed = TRUE;
        }
    }
}

int bgp_vrf_export_process_pending(bgp_instance_t *vpn_inst)
{
    if (!vpn_inst || !vpn_inst->vrf_export_state)
    {
        return 0;
    }
    bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)vpn_inst->vrf_export_state;
    st->failed_count = 0u;
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
    /* 退避窗口内 queue_process 返回 0；只要 pending 仍非空，
     * 同步 drain 就必须报告“未收敛”，不能把延时重试误当成功。 */
    return (st->failed_count > 0u || st->pending_count > 0u) ? -1 : total;
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
    if ((src_inst->afi != BGP_AFI_IPV4 && src_inst->afi != BGP_AFI_IPV6) || src_inst->safi != BGP_SAFI_UNICAST)
    {
        return;
    }
    if (!src_inst->vrf || src_inst->vrf->vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return;
    }
    bgp_instance_t *targets[2] = {0};
    targets[0] = bgp_vrf_export_target_inst_by_af(src_inst->afi, BGP_SAFI_VPN_UNICAST);
    if (src_inst->afi == BGP_AFI_IPV4)
    {
        targets[1] = bgp_vrf_export_target_inst_by_af(BGP_AFI_L2VPN, BGP_SAFI_EVPN);
    }
    gboolean queued = FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(targets); i++)
    {
        bgp_instance_t *target = targets[i];
        if (!target)
        {
            continue;
        }
        bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)target->vrf_export_state;
        if (!st || !st->enabled)
        {
            continue;
        }
        bgp_vrf_export_push_head(st, head);
        queued = TRUE;
    }
    if (queued)
    {
        (void)bgp_worker_post_vrf_export_event();
    }
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
    if (st->enabled)
    {
        return 0;
    }
    st->enabled = TRUE;
    st->failed_count = 0u;

    /* 遍历所有私网 VRF 的 matching unicast inst，全部 head 入 pending */
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
        bgp_afi_t source_afi = bgp_vrf_export_is_evpn_target(vpn_inst) ? BGP_AFI_IPV4 : vpn_inst->afi;
        bgp_instance_t *uc_inst =
            (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(source_afi, BGP_SAFI_UNICAST));
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

typedef struct
{
    bgp_instance_t *vpn_inst;
    int processed;
    gboolean failed;
} bgp_vrf_export_reprocess_ctx_t;

static gboolean bgp_vrf_export_reprocess_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    bgp_vrf_export_reprocess_ctx_t *ctx = (bgp_vrf_export_reprocess_ctx_t *)user_data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;
    if (!ctx || !ctx->vpn_inst || !head)
    {
        return FALSE;
    }
    int rc = bgp_vrf_export_process_one(ctx->vpn_inst, head);
    if (rc < 0)
    {
        ctx->failed = TRUE;
    }
    else
    {
        ctx->processed += rc;
    }
    return FALSE;
}

static void bgp_vrf_export_reprocess_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib,
                                            gpointer user_data)
{
    (void)inst;
    (void)entry;
    if (rib && rib->head_tree)
    {
        g_tree_foreach(rib->head_tree, bgp_vrf_export_reprocess_head_cb, user_data);
    }
}

int bgp_vrf_export_reprocess_instance(bgp_instance_t *src_inst)
{
    if (!src_inst || !src_inst->vrf || src_inst->vrf->vrf_id == BGP_VRF_PUBLIC_ID ||
        (src_inst->afi != BGP_AFI_IPV4 && src_inst->afi != BGP_AFI_IPV6) || src_inst->safi != BGP_SAFI_UNICAST)
    {
        return -1;
    }
    bgp_instance_t *vpn_inst = bgp_vrf_export_target_inst_by_af(src_inst->afi, BGP_SAFI_VPN_UNICAST);
    if (!vpn_inst)
    {
        return 0;
    }

    bgp_vrf_export_reprocess_ctx_t ctx = {.vpn_inst = vpn_inst};
    bgp_inst_foreach_rib(src_inst, bgp_vrf_export_reprocess_rib_cb, &ctx);
    return ctx.failed ? -1 : ctx.processed;
}

void bgp_vrf_export_backfill_vrf(uint32_t vrf_id)
{
    if (vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return;
    }
    bgp_instance_t *targets[] = {
        bgp_vrf_export_target_inst_by_af(BGP_AFI_IPV4, BGP_SAFI_VPN_UNICAST),
        bgp_vrf_export_target_inst_by_af(BGP_AFI_IPV6, BGP_SAFI_VPN_UNICAST),
        bgp_vrf_export_target_inst_by_af(BGP_AFI_L2VPN, BGP_SAFI_EVPN),
    };
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
    gboolean queued = FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(targets); i++)
    {
        bgp_instance_t *target = targets[i];
        if (!target)
        {
            continue;
        }
        bgp_afi_t source_afi = bgp_vrf_export_is_evpn_target(target) ? BGP_AFI_IPV4 : target->afi;
        bgp_instance_t *uc_inst =
            (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(source_afi, BGP_SAFI_UNICAST));
        if (!uc_inst)
        {
            continue;
        }
        bgp_vrf_export_state_t *st = (bgp_vrf_export_state_t *)target->vrf_export_state;
        if (!st || !st->enabled)
        {
            continue;
        }
        uint32_t before = st->pending_count;
        bgp_inst_foreach_rib(uc_inst, bgp_vrf_export_scan_rib_cb, st);
        if (st->pending_count > before)
        {
            LOG_INFO("BGP vrf-export: backfill VRF %u queued %u source heads afi=%u safi=%u (VRF config changed)",
                     vrf_id, st->pending_count - before, (unsigned)target->afi, (unsigned)target->safi);
            queued = TRUE;
        }
    }
    if (queued)
    {
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
    /* 先关生产门，再清 pending/RIB；同一 worker 后续 calc 不得重建
     * 已 quiesce 的 VPN 路由，更不能在 LocalSID 释放后重新申请。 */
    st->enabled = FALSE;

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
    bgp_vrf_export_retry_reset(st);

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

static gboolean bgp_vrf_export_is_private_source_inst(const bgp_instance_t *src_inst)
{
    return src_inst && src_inst->vrf && src_inst->vrf->vrf_id != BGP_VRF_PUBLIC_ID &&
           (src_inst->afi == BGP_AFI_IPV4 || src_inst->afi == BGP_AFI_IPV6) && src_inst->safi == BGP_SAFI_UNICAST;
}

/** Drop queued source heads before withdrawing their current exports.  This
 * prevents an old-locator task from recreating the route between withdraw and
 * SID release, and removes borrowed heads before source-instance teardown. */
static void bgp_vrf_export_drop_pending_source(bgp_instance_t *target, const bgp_instance_t *src_inst)
{
    bgp_vrf_export_state_t *st = target ? (bgp_vrf_export_state_t *)target->vrf_export_state : NULL;
    if (!st || !st->pending || !src_inst)
    {
        return;
    }

    for (GList *link = st->pending->head; link;)
    {
        GList *next = link->next;
        bgp_rthead_t *head = (bgp_rthead_t *)link->data;
        if (head && head->inst == src_inst)
        {
            g_queue_delete_link(st->pending, link);
            if (st->pending_count > 0u)
            {
                st->pending_count--;
            }
            bgp_rib_head_unref(head);
        }
        link = next;
    }
    if (st->pending_count == 0u)
    {
        bgp_vrf_export_retry_reset(st);
    }
}

static void bgp_vrf_export_purge_source_target(bgp_instance_t *src_inst, bgp_instance_t *target)
{
    if (!bgp_vrf_export_is_private_source_inst(src_inst) || !target)
    {
        return;
    }

    bgp_vrf_export_drop_pending_source(target, src_inst);
    GPtrArray *victims = g_ptr_array_new();
    bgp_vrf_export_purge_ctx_t ctx = {.src_inst = src_inst, .victims = victims};
    bgp_inst_foreach_rib(target, bgp_vrf_export_collect_victims_cb, &ctx);

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
        if (tgt_rib && bgp_rib_unreach_one(tgt_rib, &nlri, &synth) == 1 && target->calc_queue)
        {
            bgp_calc_queue_push(target->calc_queue, target, &nlri);
        }
    }
    if (victims->len > 0)
    {
        LOG_INFO("BGP vrf-export: purged %u routes sourced from VRF %u afi=%u safi=%u", victims->len,
                 src_inst->vrf->vrf_id, (unsigned)target->afi, (unsigned)target->safi);
    }
    g_ptr_array_free(victims, TRUE);
}

void bgp_vrf_export_purge_source_vpn_inst(bgp_instance_t *src_inst)
{
    if (!bgp_vrf_export_is_private_source_inst(src_inst))
    {
        return;
    }
    bgp_vrf_export_purge_source_target(src_inst, bgp_vrf_export_target_inst_by_af(src_inst->afi, BGP_SAFI_VPN_UNICAST));
}

void bgp_vrf_export_purge_source_inst(bgp_instance_t *src_inst)
{
    if (!bgp_vrf_export_is_private_source_inst(src_inst))
    {
        return;
    }
    bgp_vrf_export_purge_source_vpn_inst(src_inst);
    if (src_inst->afi == BGP_AFI_IPV4)
    {
        bgp_vrf_export_purge_source_target(src_inst, bgp_vrf_export_target_inst_by_af(BGP_AFI_L2VPN, BGP_SAFI_EVPN));
    }
}

int bgp_vrf_export_process_all_pending(void)
{
    int total = 0;
    gboolean failed = FALSE;
    bgp_instance_t *targets[] = {
        bgp_vrf_export_target_inst_by_af(BGP_AFI_IPV4, BGP_SAFI_VPN_UNICAST),
        bgp_vrf_export_target_inst_by_af(BGP_AFI_IPV6, BGP_SAFI_VPN_UNICAST),
        bgp_vrf_export_target_inst_by_af(BGP_AFI_L2VPN, BGP_SAFI_EVPN),
    };
    for (size_t i = 0; i < G_N_ELEMENTS(targets); i++)
    {
        int rc = bgp_vrf_export_process_pending(targets[i]);
        if (rc < 0)
        {
            failed = TRUE;
        }
        else
        {
            total += rc;
        }
    }
    return failed ? -1 : total;
}
