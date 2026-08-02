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
#include "bgp_import_route.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_nexthop.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_route_flush.h"
#include "bgp_session.h"
#include "bgp_update_group.h"
#include "bgp_vrf.h"
#include "bgp_vrf_export.h"
#include "bgp_vrf_import.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "route.h"
#include "vrf.h"

/* 在配置删除路径中，同步抽干 work 队列的最大轮次，避免销毁前遗留待撤销任务。 */
#define BGP_CFG_DRAIN_MAX_PASSES 1024u
#define BGP_CFG_DRAIN_MAX_STALLED_PASSES 8u

static int bgp_cfg_release_srv6_af(bgp_protocol_t *proto, bgp_afi_t afi);
static int bgp_cfg_prepare_srv6_af(bgp_protocol_t *proto, bgp_afi_t afi);
static int bgp_cfg_prepare_vrf_srv6_sids(bgp_vrf_t *vrf);
static int bgp_cfg_prepare_protocol_srv6_sids(bgp_protocol_t *proto);
static uint32_t bgp_cfg_drain_instance_work(bgp_instance_t *inst);

static uint32_t bgp_cfg_instance_pending_work(const bgp_instance_t *inst)
{
    return bgp_instance_pending_count(inst);
}

static bgp_instance_t *bgp_cfg_public_vpn_instance(bgp_protocol_t *proto, bgp_afi_t afi)
{
    if (!proto || !proto->vrf_hash || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return NULL;
    }
    bgp_vrf_t *pub = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
    return (pub && pub->inst_hash) ? g_hash_table_lookup(pub->inst_hash, bgp_inst_hash_key(afi, BGP_SAFI_VPN_UNICAST))
                                   : NULL;
}

/* 先把引用 service SID 的本地 VPN 路由撤销并同步抽干发布队列，随后才允许
 * 调用方释放 End.DT LocalSID，避免远端仍持有广告时本地 SID 已失效。 */
static int bgp_cfg_quiesce_vpn_exports(bgp_instance_t *inst)
{
    if (!inst)
    {
        return ERRCODE_SUCCESS;
    }
    if (bgp_vrf_export_disable(inst) != 0)
    {
        return ERRCODE_FAIL;
    }
    bgp_cfg_drain_instance_work(inst);
    return bgp_cfg_instance_pending_work(inst) == 0u ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

/* release 或后续配置步骤失败时，按当前 AF 配置重新构建本地 VPN 导出。 */
static int bgp_cfg_restore_vpn_exports(bgp_instance_t *inst)
{
    if (!inst)
    {
        return ERRCODE_SUCCESS;
    }
    int rc = bgp_vrf_export_enable(inst);
    if (rc == 0 && bgp_vrf_export_process_pending(inst) < 0)
    {
        rc = -1;
    }
    bgp_cfg_drain_instance_work(inst);
    return rc == 0 ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static bgp_instance_t *bgp_cfg_public_export_instance(bgp_protocol_t *proto, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!proto || !proto->vrf_hash)
    {
        return NULL;
    }
    bgp_vrf_t *pub = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
    return (pub && pub->inst_hash) ? g_hash_table_lookup(pub->inst_hash, bgp_inst_hash_key(afi, safi)) : NULL;
}

static int bgp_cfg_drain_public_exports(bgp_protocol_t *proto)
{
    bgp_instance_t *instances[] = {
        bgp_cfg_public_vpn_instance(proto, BGP_AFI_IPV4),
        bgp_cfg_public_vpn_instance(proto, BGP_AFI_IPV6),
        bgp_cfg_public_export_instance(proto, BGP_AFI_L2VPN, BGP_SAFI_EVPN),
    };
    int rc = ERRCODE_SUCCESS;
    for (guint i = 0; i < G_N_ELEMENTS(instances); ++i)
    {
        bgp_cfg_drain_instance_work(instances[i]);
        if (bgp_instance_pending_count(instances[i]) != 0u)
        {
            rc = ERRCODE_FAIL;
        }
    }
    return rc;
}

static void bgp_cfg_restore_vrf_exports(bgp_protocol_t *proto, uint32_t vrf_id)
{
    bgp_vrf_export_backfill_vrf(vrf_id);
    (void)bgp_vrf_export_process_all_pending();
    (void)bgp_cfg_drain_public_exports(proto);
}

/* Locator belongs to one private unicast AF.  Withdraw and rebuild only that
 * source's same-AF VPN routes; unrelated VRFs and EVPN exports stay intact. */
static int bgp_cfg_quiesce_source_vpn_exports(bgp_protocol_t *proto, bgp_instance_t *src_inst)
{
    if (!src_inst)
    {
        return ERRCODE_FAIL;
    }
    bgp_instance_t *vpn_inst = bgp_cfg_public_vpn_instance(proto, src_inst->afi);
    bgp_vrf_export_purge_source_vpn_inst(src_inst);
    bgp_cfg_drain_instance_work(vpn_inst);
    return !vpn_inst || bgp_cfg_instance_pending_work(vpn_inst) == 0u ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int bgp_cfg_restore_source_vpn_exports(bgp_protocol_t *proto, bgp_instance_t *src_inst)
{
    if (!src_inst)
    {
        return ERRCODE_FAIL;
    }
    bgp_instance_t *vpn_inst = bgp_cfg_public_vpn_instance(proto, src_inst->afi);
    int rc = bgp_vrf_export_reprocess_instance(src_inst);
    bgp_cfg_drain_instance_work(vpn_inst);
    return rc >= 0 && (!vpn_inst || bgp_cfg_instance_pending_work(vpn_inst) == 0u) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static uint32_t bgp_cfg_drain_instance_work(bgp_instance_t *inst)
{
    if (!inst)
    {
        return 0u;
    }

    uint32_t total_processed = 0u;
    uint32_t pending_before = bgp_cfg_instance_pending_work(inst);
    for (uint32_t pass = 0; pass < BGP_CFG_DRAIN_MAX_PASSES; ++pass)
    {
        if (pending_before == 0u)
        {
            return total_processed;
        }
        uint32_t processed = bgp_instance_drain_pending(inst);
        total_processed += processed;
        uint32_t pending_after = bgp_cfg_instance_pending_work(inst);
        if (pending_after == 0u)
        {
            return total_processed;
        }
        if (processed == 0u || pending_after >= pending_before)
        {
            /* RPC/依赖失败时 processed=0，立即停止。若工作只是迁移到
             * 其他 instance，protocol fixed-point 会在下一全局轮继续。 */
            return total_processed;
        }
        pending_before = pending_after;
    }
    return total_processed;
}

static int bgp_cfg_withdraw_instance_routes_sync(bgp_instance_t *inst)
{
    if (!inst)
    {
        return ERRCODE_SUCCESS;
    }
    if (bgp_cfg_instance_pending_work(inst) != 0u)
    {
        return ERRCODE_FAIL;
    }
    return bgp_route_flush_withdraw_instance_sync(inst);
}

static int bgp_cfg_withdraw_vrf_routes_sync(bgp_vrf_t *vrf)
{
    if (!vrf || !vrf->inst_hash)
    {
        return ERRCODE_SUCCESS;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, vrf->inst_hash);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        if (bgp_cfg_withdraw_instance_routes_sync((bgp_instance_t *)value) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }
    return ERRCODE_SUCCESS;
}

static int bgp_cfg_withdraw_protocol_routes_sync(bgp_protocol_t *proto)
{
    if (!proto || !proto->vrf_hash)
    {
        return ERRCODE_SUCCESS;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, proto->vrf_hash);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        if (bgp_cfg_withdraw_vrf_routes_sync((bgp_vrf_t *)value) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }
    return ERRCODE_SUCCESS;
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

static uint64_t bgp_cfg_protocol_pending_work(const bgp_protocol_t *proto)
{
    if (!proto || !proto->vrf_hash)
    {
        return 0u;
    }

    uint64_t pending = 0u;
    GHashTableIter vrf_iter;
    gpointer vrf_key = NULL;
    gpointer vrf_val = NULL;
    g_hash_table_iter_init(&vrf_iter, proto->vrf_hash);
    while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
    {
        (void)vrf_key;
        const bgp_vrf_t *vrf = (const bgp_vrf_t *)vrf_val;
        if (!vrf || !vrf->inst_hash)
        {
            continue;
        }

        GHashTableIter inst_iter;
        gpointer inst_key = NULL;
        gpointer inst_val = NULL;
        g_hash_table_iter_init(&inst_iter, vrf->inst_hash);
        while (g_hash_table_iter_next(&inst_iter, &inst_key, &inst_val))
        {
            (void)inst_key;
            pending += bgp_cfg_instance_pending_work((const bgp_instance_t *)inst_val);
        }
    }
    return pending;
}

static uint64_t bgp_cfg_drain_protocol_pass(bgp_protocol_t *proto)
{
    if (!proto || !proto->vrf_hash)
    {
        return 0u;
    }

    uint64_t processed = 0u;
    GHashTableIter vrf_iter;
    gpointer vrf_key = NULL;
    gpointer vrf_val = NULL;
    g_hash_table_iter_init(&vrf_iter, proto->vrf_hash);
    while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
    {
        (void)vrf_key;
        bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
        if (!vrf || !vrf->inst_hash)
        {
            continue;
        }

        GHashTableIter inst_iter;
        gpointer inst_key = NULL;
        gpointer inst_val = NULL;
        g_hash_table_iter_init(&inst_iter, vrf->inst_hash);
        while (g_hash_table_iter_next(&inst_iter, &inst_key, &inst_val))
        {
            (void)inst_key;
            processed += bgp_cfg_drain_instance_work((bgp_instance_t *)inst_val);
        }
    }
    return processed;
}

/* calc/import-rib/vrf-import/export 都可能把任务投到另一个 instance。
 * 因此销毁整个协议时必须跨所有 VRF 反复求 fixed-point，不能依赖哈希遍历顺序。 */
static int bgp_cfg_drain_protocol_work(bgp_protocol_t *proto)
{
    uint64_t pending_before = bgp_cfg_protocol_pending_work(proto);
    uint32_t stalled_passes = 0u;
    for (uint32_t pass = 0u; pass < BGP_CFG_DRAIN_MAX_PASSES; ++pass)
    {
        if (pending_before == 0u)
        {
            return ERRCODE_SUCCESS;
        }

        uint64_t processed = bgp_cfg_drain_protocol_pass(proto);
        uint64_t pending_after = bgp_cfg_protocol_pending_work(proto);
        if (pending_after == 0u)
        {
            return ERRCODE_SUCCESS;
        }
        if (processed == 0u)
        {
            LOG_WARN("BGP: protocol pending drain made no progress (pending=%" G_GUINT64_FORMAT ")", pending_after);
            return ERRCODE_FAIL;
        }

        if (pending_after >= pending_before)
        {
            stalled_passes++;
            if (stalled_passes >= BGP_CFG_DRAIN_MAX_STALLED_PASSES)
            {
                LOG_WARN("BGP: protocol pending drain did not converge (pending=%" G_GUINT64_FORMAT ")", pending_after);
                return ERRCODE_FAIL;
            }
        }
        else
        {
            stalled_passes = 0u;
        }
        pending_before = pending_after;
    }

    LOG_WARN("BGP: protocol pending drain exceeded %u passes (pending=%" G_GUINT64_FORMAT ")", BGP_CFG_DRAIN_MAX_PASSES,
             bgp_cfg_protocol_pending_work(proto));
    return ERRCODE_FAIL;
}

static void bgp_cfg_cleanup_vrf_import_routes(bgp_vrf_t *vrf)
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
        (void)bgp_import_route_cleanup_instance_all((bgp_instance_t *)inst_val);
    }
}

static gboolean bgp_cfg_resolve_vrf_id(bgp_apply_cmd_t *apply, uint32_t *vrf_id_out)
{
    if (!apply || !vrf_id_out || apply->vrf_name[0] == '\0')
    {
        if (apply)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Missing VRF name.");
        }
        return FALSE;
    }

    if (strcmp(apply->vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        *vrf_id_out = BGP_VRF_PUBLIC_ID;
        return TRUE;
    }

    const vrf_api_cache_entry_t *entry = vrf_api_cache_lookup_by_name(apply->vrf_name);
    if (!entry)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return FALSE;
    }

    *vrf_id_out = entry->vrf_id;
    return TRUE;
}

static bgp_vrf_t *bgp_cfg_lookup_vrf(bgp_protocol_t *proto, bgp_apply_cmd_t *apply, uint32_t *vrf_id_out)
{
    uint32_t vrf_id = 0;
    if (!proto || !apply || !bgp_cfg_resolve_vrf_id(apply, &vrf_id))
    {
        return NULL;
    }
    if (vrf_id_out)
    {
        *vrf_id_out = vrf_id;
    }
    return bgp_protocol_get_vrf(proto, vrf_id);
}

static int bgp_cfg_vrf_af_has_rd(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    if (vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return 1;
    }
    if (safi != BGP_SAFI_UNICAST)
    {
        return 0;
    }

    uint16_t vafi = (afi == BGP_AFI_IPV6) ? VRF_AFI_IPV6 : VRF_AFI_IPV4;
    const vrf_api_af_t *af = vrf_api_cache_get_af(vrf_id, vafi);
    return af && af->has_rd;
}

static void bgp_cfg_stop_vrf_sessions(bgp_vrf_t *vrf)
{
    if (!vrf)
    {
        return;
    }

    bgp_cfg_cleanup_vrf_import_routes(vrf);

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
}

static void bgp_cfg_stop_vrf_sessions_and_drain_work(bgp_vrf_t *vrf)
{
    bgp_cfg_stop_vrf_sessions(vrf);
    bgp_cfg_drain_vrf_work(vrf);
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

        /* 第一阶段只停止生产者。若边停边按 VRF drain，后访问 VRF 的
         * calc 可能把跨实例任务重新投给已经 drain 过的目标。 */
        bgp_cfg_stop_vrf_sessions(vrf);
    }

    (void)bgp_cfg_drain_protocol_work(proto);
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
        bgp_instance_t *vpn4 = bgp_cfg_public_vpn_instance(proto, BGP_AFI_IPV4);
        bgp_instance_t *vpn6 = bgp_cfg_public_vpn_instance(proto, BGP_AFI_IPV6);
        bgp_instance_t *evpn = bgp_cfg_public_export_instance(proto, BGP_AFI_L2VPN, BGP_SAFI_EVPN);
        if (bgp_cfg_quiesce_vpn_exports(vpn4) != ERRCODE_SUCCESS ||
            bgp_cfg_quiesce_vpn_exports(vpn6) != ERRCODE_SUCCESS ||
            bgp_cfg_quiesce_vpn_exports(evpn) != ERRCODE_SUCCESS)
        {
            (void)bgp_cfg_restore_vpn_exports(vpn4);
            (void)bgp_cfg_restore_vpn_exports(vpn6);
            (void)bgp_cfg_restore_vpn_exports(evpn);
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     "BGP Error: Failed to withdraw VPN exports before removing BGP.");
            return;
        }
        const int release_v4_rc = bgp_cfg_release_srv6_af(proto, BGP_AFI_IPV4);
        const int release_v6_rc = bgp_cfg_release_srv6_af(proto, BGP_AFI_IPV6);
        if (release_v4_rc != ERRCODE_SUCCESS || release_v6_rc != ERRCODE_SUCCESS)
        {
            const int prepare_rc = bgp_cfg_prepare_protocol_srv6_sids(proto);
            const int restore_v4_rc = bgp_cfg_restore_vpn_exports(vpn4);
            const int restore_v6_rc = bgp_cfg_restore_vpn_exports(vpn6);
            const int restore_evpn_rc = bgp_cfg_restore_vpn_exports(evpn);
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     (prepare_rc == ERRCODE_SUCCESS && restore_v4_rc == ERRCODE_SUCCESS &&
                      restore_v6_rc == ERRCODE_SUCCESS && restore_evpn_rc == ERRCODE_SUCCESS)
                         ? "BGP Error: Failed to release SRv6 SIDs before removing BGP; previous state was restored."
                         : "BGP Error: Failed to release SRv6 SIDs before removing BGP and rollback is incomplete.");
            return;
        }
        if (bgp_bmp_dispatch_clear_all() != 0)
        {
            const int prepare_rc = bgp_cfg_prepare_protocol_srv6_sids(proto);
            const int restore_v4_rc = bgp_cfg_restore_vpn_exports(vpn4);
            const int restore_v6_rc = bgp_cfg_restore_vpn_exports(vpn6);
            const int restore_evpn_rc = bgp_cfg_restore_vpn_exports(evpn);
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     (prepare_rc == ERRCODE_SUCCESS && restore_v4_rc == ERRCODE_SUCCESS &&
                      restore_v6_rc == ERRCODE_SUCCESS && restore_evpn_rc == ERRCODE_SUCCESS)
                         ? "BGP Error: Failed to clear BMP instances; SRv6 and VPN exports were restored."
                         : "BGP Error: Failed to clear BMP instances and rollback is incomplete.");
            return;
        }
        bgp_listen_stop();
        bgp_cfg_stop_all_sessions_and_drain_work(proto);
        bgp_import_route_unsubscribe_protocol_imports(proto);
        if (bgp_cfg_withdraw_protocol_routes_sync(proto) != ERRCODE_SUCCESS)
        {
            const int prepare_rc = bgp_cfg_prepare_protocol_srv6_sids(proto);
            const int restore_v4_rc = bgp_cfg_restore_vpn_exports(vpn4);
            const int restore_v6_rc = bgp_cfg_restore_vpn_exports(vpn6);
            const int restore_evpn_rc = bgp_cfg_restore_vpn_exports(evpn);
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     (prepare_rc == ERRCODE_SUCCESS && restore_v4_rc == ERRCODE_SUCCESS &&
                      restore_v6_rc == ERRCODE_SUCCESS && restore_evpn_rc == ERRCODE_SUCCESS)
                         ? "BGP Error: Pending work did not converge or ROUTE did not acknowledge all withdrawals; "
                           "BGP was not removed and its SRv6/VPN export state was restored."
                         : "BGP Error: Pending work did not converge or ROUTE did not acknowledge all withdrawals; "
                           "BGP was not removed and rollback is incomplete.");
            return;
        }
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

static int bgp_cfg_prepare_vrf_srv6_af(bgp_vrf_t *vrf, bgp_afi_t afi)
{
    if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID || !vrf->inst_hash || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return ERRCODE_SUCCESS;
    }
    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, BGP_SAFI_UNICAST));
    if (!inst || inst->srv6_locator[0] == '\0')
    {
        return ERRCODE_SUCCESS;
    }
    return bgp_vrf_export_prepare_srv6_sid(inst);
}

static int bgp_cfg_prepare_srv6_af(bgp_protocol_t *proto, bgp_afi_t afi)
{
    if (!proto || !proto->vrf_hash || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return ERRCODE_FAIL;
    }
    int rc = ERRCODE_SUCCESS;
    GHashTableIter it;
    gpointer value = NULL;
    g_hash_table_iter_init(&it, proto->vrf_hash);
    while (g_hash_table_iter_next(&it, NULL, &value))
    {
        if (bgp_cfg_prepare_vrf_srv6_af((bgp_vrf_t *)value, afi) != ERRCODE_SUCCESS)
        {
            rc = ERRCODE_FAIL;
        }
    }
    return rc;
}

static int bgp_cfg_prepare_vrf_srv6_sids(bgp_vrf_t *vrf)
{
    const int v4_rc = bgp_cfg_prepare_vrf_srv6_af(vrf, BGP_AFI_IPV4);
    const int v6_rc = bgp_cfg_prepare_vrf_srv6_af(vrf, BGP_AFI_IPV6);
    return v4_rc == ERRCODE_SUCCESS && v6_rc == ERRCODE_SUCCESS ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int bgp_cfg_prepare_protocol_srv6_sids(bgp_protocol_t *proto)
{
    const int v4_rc = bgp_cfg_prepare_srv6_af(proto, BGP_AFI_IPV4);
    const int v6_rc = bgp_cfg_prepare_srv6_af(proto, BGP_AFI_IPV6);
    return v4_rc == ERRCODE_SUCCESS && v6_rc == ERRCODE_SUCCESS ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int bgp_cfg_release_srv6_af(bgp_protocol_t *proto, bgp_afi_t afi)
{
    if (!proto || !proto->vrf_hash || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return ERRCODE_FAIL;
    }
    int rc = ERRCODE_SUCCESS;
    GHashTableIter it;
    gpointer value = NULL;
    g_hash_table_iter_init(&it, proto->vrf_hash);
    while (g_hash_table_iter_next(&it, NULL, &value))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)value;
        if (vrf && vrf->vrf_id != BGP_VRF_PUBLIC_ID && bgp_vrf_export_release_srv6_sid(vrf, afi) != ERRCODE_SUCCESS)
        {
            rc = ERRCODE_FAIL;
        }
    }
    if (rc != ERRCODE_SUCCESS && bgp_cfg_prepare_srv6_af(proto, afi) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: failed to compensate partial SRv6 SID release for afi=%u", (unsigned)afi);
    }
    return rc;
}

void bgp_cfg_apply_srv6(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP is not configured.");
        return;
    }
    if (apply->vrf_name[0] == '\0' || strcmp(apply->vrf_name, VRF_PUBLIC_VRF_NAME) == 0 ||
        apply->u.srv6.safi != BGP_SAFI_UNICAST ||
        (apply->u.srv6.afi != BGP_AFI_IPV4 && apply->u.srv6.afi != BGP_AFI_IPV6))
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: SRv6 requires a private unicast address-family.");
        return;
    }
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
    bgp_instance_t *inst =
        vrf && vrf->inst_hash
            ? g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(apply->u.srv6.afi, BGP_SAFI_UNICAST))
            : NULL;
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Private unicast address-family is not enabled.");
        return;
    }
    const char *next = apply->isNo ? "" : apply->u.srv6.locator;
    if (!apply->isNo && next[0] == '\0')
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: SRv6 locator is required.");
        return;
    }
    if (next[0] != '\0' &&
        (!g_bgp_local || !g_bgp_local->dev_ipc_ctx ||
         srv6_rpc_locator_exists(g_bgp_local->dev_ipc_ctx, next, SRV6_RPC_DEFAULT_TIMEOUT_MS) != ERRCODE_SUCCESS))
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: SRv6 locator does not exist or is unavailable.");
        return;
    }

    if (strcmp(inst->srv6_locator, next) == 0)
    {
        /* A matching configuration is not proof that the volatile cache and
         * SRV6 LocalSID survived a restart.  Re-run the stable-key operation:
         * alloc repairs a configured SID, while an empty locator removes any
         * owner-scope orphan left by an interrupted delete. */
        int repair_rc = next[0] == '\0' ? bgp_vrf_export_reconcile_srv6_sid_absent(vrf, apply->u.srv6.afi)
                                        : bgp_vrf_export_prepare_srv6_sid(inst);
        if (repair_rc != ERRCODE_SUCCESS)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     next[0] == '\0' ? "BGP Error: Failed to reconcile the absent SRv6 locator."
                                     : "BGP Error: Failed to repair the End.DT SID for the SRv6 locator.");
            return;
        }
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    char old_locator[SRV6_LOCATOR_NAME_MAX];
    g_strlcpy(old_locator, inst->srv6_locator, sizeof(old_locator));

    /* Locator owns one End.DT SID for this source VRF/AF.  Withdraw this
     * source's VPN exports before replacing the LocalSID so no peer can retain
     * an advertisement that refers to a released SID. */
    if (bgp_cfg_quiesce_source_vpn_exports(proto, inst) != ERRCODE_SUCCESS)
    {
        int restore_rc = bgp_cfg_restore_source_vpn_exports(proto, inst);
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 restore_rc == ERRCODE_SUCCESS
                     ? "BGP Error: Failed to withdraw the VRF's VPN exports; previous exports were restored."
                     : "BGP Error: Failed to withdraw the VRF's VPN exports and rollback is pending reconciliation.");
        return;
    }
    if (inst->srv6_locator[0] != '\0' && bgp_vrf_export_release_srv6_sid(vrf, apply->u.srv6.afi) != ERRCODE_SUCCESS)
    {
        int prepare_rc = bgp_vrf_export_prepare_srv6_sid(inst);
        int restore_rc = bgp_cfg_restore_source_vpn_exports(proto, inst);
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 (prepare_rc == ERRCODE_SUCCESS && restore_rc == ERRCODE_SUCCESS)
                     ? "BGP Error: Failed to release the VRF's SRv6 SID; previous locator was restored."
                     : "BGP Error: Failed to release the VRF's SRv6 SID and rollback is pending reconciliation.");
        return;
    }

    g_strlcpy(inst->srv6_locator, next, sizeof(inst->srv6_locator));
    if (next[0] != '\0' && bgp_vrf_export_prepare_srv6_sid(inst) != ERRCODE_SUCCESS)
    {
        int cleanup_rc = bgp_vrf_export_reconcile_srv6_sid_absent(vrf, apply->u.srv6.afi);
        g_strlcpy(inst->srv6_locator, old_locator, sizeof(inst->srv6_locator));
        int prepare_rc = old_locator[0] == '\0' ? ERRCODE_SUCCESS : bgp_vrf_export_prepare_srv6_sid(inst);
        int restore_rc = bgp_cfg_restore_source_vpn_exports(proto, inst);
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 (cleanup_rc == ERRCODE_SUCCESS && prepare_rc == ERRCODE_SUCCESS && restore_rc == ERRCODE_SUCCESS)
                     ? "BGP Error: Failed to prepare the End.DT SID; previous locator was restored."
                     : "BGP Error: Failed to prepare the End.DT SID and rollback is pending reconciliation.");
        return;
    }

    /* The shared VPN Loc-RIB remains Prefix-SID neutral.  During drain, each
     * update group selects MPLS or the cached End.DT SID independently. */
    if (bgp_cfg_restore_source_vpn_exports(proto, inst) != ERRCODE_SUCCESS)
    {
        (void)bgp_cfg_quiesce_source_vpn_exports(proto, inst);
        int cleanup_rc =
            next[0] == '\0' ? ERRCODE_SUCCESS : bgp_vrf_export_reconcile_srv6_sid_absent(vrf, apply->u.srv6.afi);
        g_strlcpy(inst->srv6_locator, old_locator, sizeof(inst->srv6_locator));
        int prepare_rc = old_locator[0] == '\0' ? ERRCODE_SUCCESS : bgp_vrf_export_prepare_srv6_sid(inst);
        int restore_rc = bgp_cfg_restore_source_vpn_exports(proto, inst);
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 (cleanup_rc == ERRCODE_SUCCESS && prepare_rc == ERRCODE_SUCCESS && restore_rc == ERRCODE_SUCCESS)
                     ? "BGP Error: Failed to rebuild VPN exports; previous locator was restored."
                     : "BGP Error: Failed to rebuild VPN exports and rollback is pending reconciliation.");
        return;
    }
    apply->rc = BGP_APPLY_RC_OK;
}

void bgp_cfg_apply_neighbor_srv6_sid(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
    bgp_afi_t afi = apply ? apply->u.neighbor_srv6_sid.afi : 0;
    bgp_safi_t safi = apply ? apply->u.neighbor_srv6_sid.safi : 0;
    if (!apply || !proto)
    {
        if (apply)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP is not configured.");
        }
        return;
    }
    if (strcmp(apply->vrf_name, VRF_PUBLIC_VRF_NAME) != 0 || safi != BGP_SAFI_VPN_UNICAST ||
        (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 "BGP Error: neighbor srv6-sid is only valid under the public VPNv4/VPNv6 address-family.");
        return;
    }
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
    bgp_instance_t *inst =
        vrf && vrf->inst_hash ? g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi)) : NULL;
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VPN address-family is not enabled.");
        return;
    }

    bgp_peer_t *peer = inst->peer_hash ? g_hash_table_lookup(inst->peer_hash, &apply->u.neighbor_srv6_sid.addr) : NULL;
    if (!peer)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Neighbor not enabled in this address family.");
        return;
    }

    const gboolean enable = apply->isNo ? FALSE : TRUE;
    const gboolean current = BIT_TEST(peer->flags, BGP_PEER_FLAG_SRV6_SID) ? TRUE : FALSE;
    if (current == enable)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.neighbor_srv6_sid.addr);
    const gboolean established = sess && sess->pri_conn && sess->fsm_state == BGP_FSM_STATE_ESTABLISHED &&
                                 peer->state == BGP_PEER_STATE_ESTABLISHED;
    if (enable && afi == BGP_AFI_IPV4 && established &&
        !bgp_session_ext_nh_negotiated(sess, (uint16_t)afi, (uint16_t)safi, BGP_AFI_IPV6))
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 "BGP Error: VPNv4 SRv6 SID advertisement requires negotiated RFC 8950 IPv6 nexthop capability.");
        return;
    }

    /* A peer changes wire format and therefore must leave its old update group.
     * Withdraw only this peer's old ARO; other peers keep the shared subgroup. */
    if (established && peer->subgroups && !bgp_update_group_withdraw_peer_aro(peer, sess))
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to withdraw the neighbor's old VPN routes.");
        return;
    }
    if (peer->subgroups)
    {
        bgp_subgroup_peer_leave(peer, sess);
    }
    if (enable)
    {
        BIT_SET(peer->flags, BGP_PEER_FLAG_SRV6_SID);
    }
    else
    {
        BIT_CLR(peer->flags, BGP_PEER_FLAG_SRV6_SID);
    }
    if (established)
    {
        (void)bgp_update_group_catchup_peer(peer, sess);
    }
    bgp_cfg_drain_instance_work(inst);

    if (bgp_instance_pending_count(inst) != 0u)
    {
        /* New group did not converge.  Withdraw any partial incarnation and
         * restore the old group key before reporting failure. */
        gboolean withdraw_ok = !established || !peer->subgroups || bgp_update_group_withdraw_peer_aro(peer, sess);
        if (peer->subgroups)
        {
            bgp_subgroup_peer_leave(peer, sess);
        }
        if (current)
        {
            BIT_SET(peer->flags, BGP_PEER_FLAG_SRV6_SID);
        }
        else
        {
            BIT_CLR(peer->flags, BGP_PEER_FLAG_SRV6_SID);
        }
        if (established)
        {
            (void)bgp_update_group_catchup_peer(peer, sess);
        }
        bgp_cfg_drain_instance_work(inst);
        const gboolean restore_ok = bgp_instance_pending_count(inst) == 0u;
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 (withdraw_ok && restore_ok)
                     ? "BGP Error: Failed to switch the neighbor's SRv6 SID update group; previous mode was restored."
                     : "BGP Error: Failed to switch the neighbor's SRv6 SID update group and rollback is incomplete.");
        return;
    }
    apply->rc = BGP_APPLY_RC_OK;
}

void bgp_cfg_apply_srv6_be(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
    bgp_afi_t afi = apply ? apply->u.srv6_mode.afi : 0;
    bgp_safi_t safi = apply ? apply->u.srv6_mode.safi : 0;
    if (!apply || !proto)
    {
        if (apply)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP is not configured.");
        }
        return;
    }
    if (apply->vrf_name[0] == '\0' || strcmp(apply->vrf_name, VRF_PUBLIC_VRF_NAME) == 0 || safi != BGP_SAFI_UNICAST ||
        (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg),
                 "BGP Error: SRv6 BE recursion requires a private unicast address-family.");
        return;
    }
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
    bgp_instance_t *inst =
        vrf && vrf->inst_hash ? g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, BGP_SAFI_UNICAST)) : NULL;
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Private unicast address-family is not enabled.");
        return;
    }

    const gboolean enable = apply->isNo ? FALSE : TRUE;
    const gboolean current = BIT_TEST(inst->flags, BGP_INST_FLAG_SRV6_BE) ? TRUE : FALSE;
    if (current == enable)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    if (enable)
    {
        BIT_SET(inst->flags, BGP_INST_FLAG_SRV6_BE);
    }
    else
    {
        BIT_CLR(inst->flags, BGP_INST_FLAG_SRV6_BE);
    }
    if (bgp_vrf_import_reprocess_target_inst(inst) != 0)
    {
        if (enable)
        {
            BIT_CLR(inst->flags, BGP_INST_FLAG_SRV6_BE);
        }
        else
        {
            BIT_SET(inst->flags, BGP_INST_FLAG_SRV6_BE);
        }
        (void)bgp_vrf_import_reprocess_target_inst(inst);
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to re-evaluate imported VPN routes.");
        return;
    }
    bgp_cfg_drain_instance_work(inst);
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * vrf <name>（BGP VRF 视图入口）
 * ========================================================================== */

void bgp_cfg_apply_vrf(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured. Run 'bgp <as-number>' first.");
        return;
    }
    if (!apply->isNo && strcmp(apply->vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: public VRF uses the base BGP view.");
        return;
    }
    uint32_t vrf_id = 0;
    if (!bgp_cfg_resolve_vrf_id(apply, &vrf_id))
    {
        return;
    }
    if (apply->isNo)
    {
        if (vrf_id == BGP_VRF_PUBLIC_ID)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: public VRF cannot be deleted.");
            return;
        }
        bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
        if (!vrf)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }

        /* 先撤销该 VRF 在公网 VPN/EVPN 表的所有导出并发完 withdraw，随后
         * 才释放 per-AF service SID。失败时 VRF 仍存在，可按原配置回灌。 */
        bgp_instance_t *uc4 =
            vrf->inst_hash ? g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV4, BGP_SAFI_UNICAST))
                           : NULL;
        bgp_instance_t *uc6 =
            vrf->inst_hash ? g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV6, BGP_SAFI_UNICAST))
                           : NULL;
        bgp_vrf_export_purge_source_inst(uc4);
        bgp_vrf_export_purge_source_inst(uc6);
        if (bgp_cfg_drain_public_exports(proto) != ERRCODE_SUCCESS)
        {
            bgp_cfg_restore_vrf_exports(proto, vrf_id);
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     "BGP Error: Failed to drain VPN withdraws before removing the VRF.");
            return;
        }
        const int release_v4_rc = bgp_vrf_export_release_srv6_sid(vrf, BGP_AFI_IPV4);
        const int release_v6_rc = bgp_vrf_export_release_srv6_sid(vrf, BGP_AFI_IPV6);
        if (release_v4_rc != ERRCODE_SUCCESS || release_v6_rc != ERRCODE_SUCCESS)
        {
            const int prepare_rc = bgp_cfg_prepare_vrf_srv6_sids(vrf);
            bgp_cfg_restore_vrf_exports(proto, vrf_id);
            snprintf(
                apply->errmsg, sizeof(apply->errmsg),
                prepare_rc == ERRCODE_SUCCESS
                    ? "BGP Error: Failed to release SRv6 SIDs before removing the VRF; previous state was restored."
                    : "BGP Error: Failed to release SRv6 SIDs before removing the VRF and rollback is incomplete.");
            return;
        }
        bgp_listen_stop_vrf(vrf);
        bgp_cfg_stop_vrf_sessions_and_drain_work(vrf);
        if (bgp_cfg_withdraw_vrf_routes_sync(vrf) != ERRCODE_SUCCESS)
        {
            const int prepare_rc = bgp_cfg_prepare_vrf_srv6_sids(vrf);
            bgp_cfg_restore_vrf_exports(proto, vrf_id);
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     prepare_rc == ERRCODE_SUCCESS
                         ? "BGP Error: Pending work did not converge or ROUTE did not acknowledge all VRF withdrawals; "
                           "the VRF was not removed and its SRv6/VPN export state was restored."
                         : "BGP Error: Pending work did not converge or ROUTE did not acknowledge all VRF withdrawals; "
                           "the VRF was not removed and rollback is incomplete.");
            return;
        }
        g_hash_table_remove(proto->vrf_hash, &vrf_id);
        apply->rc = BGP_APPLY_RC_OK;
        return;
    }
    if (bgp_protocol_get_vrf(proto, vrf_id))
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_or_create_vrf(proto, vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }
    bgp_listen_start_vrf(vrf);
    bgp_vrf_import_backfill();

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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }
    if (!is_no && !bgp_cfg_vrf_af_has_rd(vrf->vrf_id, apply->u.instance.afi, apply->u.instance.safi))
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF address-family RD is not configured.");
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
        const gboolean public_vpn = vrf->vrf_id == BGP_VRF_PUBLIC_ID &&
                                    apply->u.instance.safi == BGP_SAFI_VPN_UNICAST &&
                                    (apply->u.instance.afi == BGP_AFI_IPV4 || apply->u.instance.afi == BGP_AFI_IPV6);
        const gboolean private_unicast =
            vrf->vrf_id != BGP_VRF_PUBLIC_ID && apply->u.instance.safi == BGP_SAFI_UNICAST &&
            (apply->u.instance.afi == BGP_AFI_IPV4 || apply->u.instance.afi == BGP_AFI_IPV6);
        /* Public VPN AF is only the route carrier.  Deleting it withdraws its
         * routes but does not release SIDs owned by private unicast AFs. */
        if (public_vpn && bgp_cfg_quiesce_vpn_exports(inst) != ERRCODE_SUCCESS)
        {
            (void)bgp_cfg_restore_vpn_exports(inst);
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     "BGP Error: Failed to withdraw VPN exports before disabling the address-family.");
            return;
        }
        if (private_unicast)
        {
            /* 私网 AF 是 End.DT SID 的实际业务所有者。先撤销它在
             * public VPN/EVPN 中的导出，再释放该 AF 的 LocalSID。 */
            bgp_vrf_export_purge_source_inst(inst);
            if (bgp_cfg_drain_public_exports(proto) != ERRCODE_SUCCESS)
            {
                bgp_cfg_restore_vrf_exports(proto, vrf->vrf_id);
                snprintf(apply->errmsg, sizeof(apply->errmsg),
                         "BGP Error: Failed to drain VPN withdraws before disabling the address-family.");
                return;
            }
        }
        if (private_unicast && bgp_vrf_export_release_srv6_sid(vrf, apply->u.instance.afi) != ERRCODE_SUCCESS)
        {
            const int prepare_rc = bgp_cfg_prepare_vrf_srv6_af(vrf, apply->u.instance.afi);
            bgp_cfg_restore_vrf_exports(proto, vrf->vrf_id);
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     prepare_rc == ERRCODE_SUCCESS
                         ? "BGP Error: Failed to release the VRF SRv6 SID before disabling the address-family; "
                           "previous state was restored."
                         : "BGP Error: Failed to release the VRF SRv6 SID before disabling the address-family and "
                           "rollback is incomplete.");
            return;
        }
        (void)bgp_import_route_cleanup_instance_all(inst);
        bgp_cfg_drain_instance_work(inst);

        if (inst->peer_hash)
        {
            GArray *peer_addrs = g_array_new(FALSE, FALSE, sizeof(net_addr_t));
            GHashTableIter iter;
            gpointer k, v;
            g_hash_table_iter_init(&iter, inst->peer_hash);
            while (g_hash_table_iter_next(&iter, &k, &v))
            {
                (void)v;
                if (k)
                {
                    net_addr_t addr = *(const net_addr_t *)k;
                    g_array_append_val(peer_addrs, addr);
                }
            }
            for (guint i = 0; i < peer_addrs->len; ++i)
            {
                const net_addr_t *addr = &g_array_index(peer_addrs, net_addr_t, i);
                (void)bgp_vrf_af_disable_neighbor(vrf, apply->u.instance.afi, apply->u.instance.safi, addr);
            }
            g_array_free(peer_addrs, TRUE);

            /* MP/Extended-NH capability 都是 OPEN 级协商；删除一个 AF 后即使
             * 邻居仍有其他 AF，也必须重建会话，不能继续使用旧能力集。 */
            bgp_vrf_reset_all_sessions(vrf);
        }
        /* public VPN 类 AF 去使能：先撤销所有已导出 VPN/EVPN 路由，再删实例 */
        if (vrf->vrf_id == BGP_VRF_PUBLIC_ID &&
            (((apply->u.instance.afi == BGP_AFI_IPV4 || apply->u.instance.afi == BGP_AFI_IPV6) &&
              apply->u.instance.safi == BGP_SAFI_VPN_UNICAST) ||
             (apply->u.instance.afi == BGP_AFI_L2VPN && apply->u.instance.safi == BGP_SAFI_EVPN)))
        {
            if (!public_vpn)
            {
                (void)bgp_vrf_export_disable(inst);
                bgp_cfg_drain_instance_work(inst);
            }
        }
        bgp_cfg_drain_instance_work(inst);
        if (bgp_cfg_withdraw_instance_routes_sync(inst) != ERRCODE_SUCCESS)
        {
            const int prepare_rc =
                private_unicast ? bgp_cfg_prepare_vrf_srv6_af(vrf, apply->u.instance.afi) : ERRCODE_SUCCESS;
            if (private_unicast)
            {
                bgp_cfg_restore_vrf_exports(proto, vrf->vrf_id);
            }
            else if (public_vpn)
            {
                (void)bgp_cfg_restore_vpn_exports(inst);
            }
            snprintf(apply->errmsg, sizeof(apply->errmsg),
                     prepare_rc == ERRCODE_SUCCESS
                         ? "BGP Error: Pending work did not converge or ROUTE did not acknowledge all address-family "
                           "withdrawals; the address-family was not removed and its SRv6/export state was restored."
                         : "BGP Error: Pending work did not converge or ROUTE did not acknowledge all address-family "
                           "withdrawals; the address-family was not removed and rollback is incomplete.");
            return;
        }
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
        /* public VPN 类 AF 使能：把所有私网 VRF 的 unicast 路由按 RD 全量导出(分批) */
        if (vrf->vrf_id == BGP_VRF_PUBLIC_ID &&
            (((apply->u.instance.afi == BGP_AFI_IPV4 || apply->u.instance.afi == BGP_AFI_IPV6) &&
              apply->u.instance.safi == BGP_SAFI_VPN_UNICAST) ||
             (apply->u.instance.afi == BGP_AFI_L2VPN && apply->u.instance.safi == BGP_SAFI_EVPN)))
        {
            (void)bgp_vrf_export_enable(inst);
        }
        if (vrf->vrf_id != BGP_VRF_PUBLIC_ID &&
            (apply->u.instance.afi == BGP_AFI_IPV4 || apply->u.instance.afi == BGP_AFI_IPV6) &&
            apply->u.instance.safi == BGP_SAFI_UNICAST)
        {
            bgp_vrf_import_backfill();
            /* 本地交叉：新建的私网 unicast 实例可能是其它 VRF 路由的泄漏目标(其 bgp_vrf_t 之前
             * 不存在、源 VRF calc 时无法 upsert 进来)。实例就绪后重评所有源，把已有路由补泄漏进来。 */
            bgp_vrf_import_local_backfill_target_vrf(vrf->vrf_id);
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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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

static uint32_t bgp_vrf_count_import_proto_afi(const bgp_vrf_t *vrf, uint32_t proto, bgp_afi_t afi)
{
    if (!vrf || !vrf->inst_hash)
    {
        return 0;
    }

    uint32_t mask = 1U << proto;
    uint32_t count = 0;
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, vrf->inst_hash);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        bgp_instance_t *inst = (bgp_instance_t *)value;
        if (inst && inst->afi == afi && (inst->import_protos & mask) != 0u)
        {
            count++;
        }
    }
    return count;
}

void bgp_cfg_apply_import_route(bgp_apply_cmd_t *apply)
{
    const gboolean is_no = apply->isNo ? TRUE : FALSE;
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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

    /* import-route static / import-route connected 互斥（覆盖式）：
     * 同一 AF 内任意时刻仅保留一种引入协议。 */
    static const uint32_t k_overwrite_protos[] = {ROUTE_PROTOCOL_STATIC, ROUTE_PROTOCOL_CONNECTED};
    uint32_t target_proto = apply->u.import_route.import_proto;
    uint32_t target_mask = 1U << target_proto;
    uint32_t prev_protos = inst->import_protos;

    /* 计算每个候选协议的 before/after 状态，便于决定是否需要订阅或取消订阅。 */
    uint32_t new_protos = prev_protos;
    if (is_no)
    {
        new_protos &= ~target_mask;
    }
    else
    {
        /* 覆盖：先清掉所有可覆盖位，再置目标位 */
        for (size_t i = 0; i < G_N_ELEMENTS(k_overwrite_protos); ++i)
        {
            new_protos &= ~(1U << k_overwrite_protos[i]);
        }
        new_protos |= target_mask;
    }
    inst->import_protos = new_protos;

    apply->out.import_protos = new_protos;

    /* 对每个被覆盖位发起 cleanup + 取消订阅；对新启用位发起订阅。
     * 订阅生命周期跟随 BGP work 内存态，避免 CLI/DB restore 各自维护 ROUTE IPC 副作用。 */
    for (size_t i = 0; i < G_N_ELEMENTS(k_overwrite_protos); ++i)
    {
        uint32_t p = k_overwrite_protos[i];
        uint32_t m = 1U << p;
        gboolean was_on = (prev_protos & m) != 0u;
        gboolean now_on = (new_protos & m) != 0u;
        if (was_on && !now_on)
        {
            (void)bgp_import_route_cleanup_instance(inst, p);
            uint32_t cnt = bgp_vrf_count_import_proto_afi(vrf, p, apply->u.import_route.afi);
            if (cnt == 0u)
            {
                (void)bgp_import_route_unsubscribe(p, vrf->vrf_id, (uint16_t)apply->u.import_route.afi);
            }
        }
    }

    if (!is_no && (new_protos & target_mask) != 0u && (prev_protos & target_mask) == 0u)
    {
        (void)bgp_import_route_subscribe(target_proto, vrf->vrf_id, (uint16_t)apply->u.import_route.afi,
                                         ROUTE_SUBSCRIBE_FLAG_FULL);
    }

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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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
 * @brief 计算 DQPN 线上长度 bit 数（8/16/24）
 */
static uint8_t qp_dqpn_wire_bits(uint32_t dqpn)
{
    if (dqpn <= 0xFFU)
    {
        return 8u;
    }
    if (dqpn <= 0xFFFFU)
    {
        return 16u;
    }
    return 24u;
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
    nlri->qp.dqpn_len = qp_dqpn_wire_bits(dqpn);
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
        if (bgp_rib_route_apply_reach(route, ROUTE_PROTOCOL_STATIC, &attr) != 0 ||
            bgp_nexthop_set_route(route, &nexthop) != ERRCODE_SUCCESS)
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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
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

void bgp_cfg_apply_vpn_target_policy(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }

    /* policy vpn-target 挂在公网 VPN 类实例上(vpnv4/未来 vpnv6/evpn)，按 apply 携带的 afi/safi 定位 */
    bgp_vrf_t *pub = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
    bgp_instance_t *inst =
        (pub && pub->inst_hash)
            ? (bgp_instance_t *)g_hash_table_lookup(
                  pub->inst_hash, bgp_inst_hash_key(apply->u.vpn_target.afi, apply->u.vpn_target.safi))
            : NULL;
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VPN address-family not enabled.");
        return;
    }

    const gboolean want = apply->isNo ? FALSE : TRUE; /* 默认(非 no)=启用过滤 */
    const gboolean cur = (inst->flags & BGP_INST_FLAG_VPN_TARGET_FILTER) ? TRUE : FALSE;
    if (cur == want)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    if (want)
    {
        inst->flags |= BGP_INST_FLAG_VPN_TARGET_FILTER;
    }
    else
    {
        inst->flags &= ~(uint32_t)BGP_INST_FLAG_VPN_TARGET_FILTER;
    }

    /* 策略变更后需让对端重传 vpnv4 路由，以新策略重新评估接收：
     *  - 关闭(want=FALSE)：之前因 IRT 不命中被丢弃的路由需重新拉回并接受；
     *  - 开启(want=TRUE) ：之前无条件接受的不命中路由需在重收时被入向过滤丢弃。 */
    bgp_vrf_import_request_refresh();
    apply->rc = BGP_APPLY_RC_OK;
}

void bgp_cfg_apply_advertise_evpn_route(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
    if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: private VRF address-family required.");
        return;
    }
    if (apply->u.advertise_evpn_route.afi != BGP_AFI_IPV4 || apply->u.advertise_evpn_route.safi != BGP_SAFI_UNICAST)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: IPv4 unicast VRF address-family required.");
        return;
    }

    bgp_instance_t *inst =
        (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV4, BGP_SAFI_UNICAST));
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: address-family not enabled.");
        return;
    }

    gboolean want = apply->isNo ? FALSE : TRUE;
    gboolean cur = BIT_TEST(inst->flags, BGP_INST_FLAG_ADVERTISE_EVPN_ROUTE) ? TRUE : FALSE;
    if (cur == want)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    if (want)
    {
        BIT_SET(inst->flags, BGP_INST_FLAG_ADVERTISE_EVPN_ROUTE);
    }
    else
    {
        BIT_CLR(inst->flags, BGP_INST_FLAG_ADVERTISE_EVPN_ROUTE);
    }

    bgp_vrf_export_backfill_vrf(vrf->vrf_id);
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * refresh bgp neighbor <ip> import|export af <afi-safi>     ← 单 peer
 * refresh bgp af <afi-safi> import|export                    ← 整 AF 所有 peer
 *
 * import：向对端发 ROUTE-REFRESH（RFC 2918），让对端重新发送其 Adj-RIB-Out
 * export：本端从 Adj-RIB-Out 重新向该邻居发送匹配 AF 的全部路由
 * ========================================================================== */

/** 对单条 session 执行一次 refresh；返回 0=ok / 失败原因码（>0=部分失败计数） */
static int bgp_refresh_one_session(bgp_session_t *sess, bgp_afi_t afi, bgp_safi_t safi, bool is_export, char *errmsg,
                                   size_t errsz)
{
    if (sess->fsm_state != BGP_FSM_STATE_ESTABLISHED || !sess->pri_conn)
    {
        snprintf(errmsg, errsz, "BGP Error: Session not established.");
        return -1;
    }
    if (is_export)
    {
        bgp_update_group_refresh_session_af(sess, (uint16_t)afi, (uint8_t)safi);
        return 0;
    }
    if (!BIT_TEST(sess->negotiated_caps, BGP_SESS_CAP_ROUTE_REFRESH))
    {
        snprintf(errmsg, errsz, "BGP Error: Route Refresh capability not negotiated.");
        return -1;
    }
    if (bgp_pkt_send_route_refresh(sess->pri_conn, (uint16_t)afi, (uint8_t)safi) != 0)
    {
        snprintf(errmsg, errsz, "BGP Error: Failed to send ROUTE-REFRESH.");
        return -1;
    }
    return 0;
}

void bgp_cfg_apply_refresh(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local->protocol;
    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }

    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    bgp_afi_t afi = apply->u.refresh.afi;
    bgp_safi_t safi = apply->u.refresh.safi;
    bool is_export = apply->u.refresh.is_export;

    /* 单 peer 模式：addr 非全零 */
    if (apply->u.refresh.addr.family != 0)
    {
        bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.refresh.addr);
        if (!sess)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Neighbor not found.");
            return;
        }
        if (bgp_refresh_one_session(sess, afi, safi, is_export, apply->errmsg, sizeof(apply->errmsg)) != 0)
        {
            return;
        }
        apply->rc = BGP_APPLY_RC_OK;
        return;
    }

    /* AF 模式：addr.family == 0，刷该 AF 下所有 peer */
    bgp_instance_t *inst = (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi));
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Address family not enabled.");
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    uint32_t ok_count = 0;
    uint32_t skip_count = 0;
    g_hash_table_iter_init(&iter, inst->peer_hash);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        bgp_peer_t *peer = (bgp_peer_t *)value;
        if (!peer)
        {
            continue;
        }
        bgp_session_t *sess = bgp_vrf_find_session(vrf, &peer->addr);
        if (!sess)
        {
            skip_count++;
            continue;
        }
        char tmp_err[128] = {0};
        if (bgp_refresh_one_session(sess, afi, safi, is_export, tmp_err, sizeof(tmp_err)) == 0)
        {
            ok_count++;
        }
        else
        {
            skip_count++;
        }
    }
    if (ok_count == 0 && skip_count > 0)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: No peers refreshed in AF (skipped=%u)", skip_count);
        return;
    }
    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * reflector cluster-id / no reflector cluster-id（RFC 4456）
 * ========================================================================== */

void bgp_cfg_apply_cluster_id(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local->protocol;
    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }
    bgp_instance_t *inst = (bgp_instance_t *)g_hash_table_lookup(
        vrf->inst_hash, bgp_inst_hash_key(apply->u.cluster_id.afi, apply->u.cluster_id.safi));
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Address family not enabled.");
        return;
    }

    uint32_t want = apply->isNo ? 0u : apply->u.cluster_id.cluster_id;
    if (inst->cluster_id == want)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }
    inst->cluster_id = want;

    /* cluster-id 变化对两侧路由都有影响，自动触发本 AF 下所有 ESTABLISHED peer：
     *   (1) 出向 soft-out：refresh_session_af 重 eval_export 用新 cluster-id 刷新 ARO，
     *       强推 announce_queue 让 client 收到新 CLUSTER_LIST
     *   (2) 入向：给协商过 RR 能力的 peer 发 ROUTE-REFRESH，让对端重传 → 本端 ingest
     *       命中新 cluster-id 的环路检测把历史环路路径转 unreach 撤销 */
    if (inst->peer_hash)
    {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, inst->peer_hash);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            (void)key;
            bgp_peer_t *peer = (bgp_peer_t *)value;
            if (!peer)
            {
                continue;
            }
            bgp_session_t *sess = bgp_vrf_find_session(vrf, &peer->addr);
            if (!sess || sess->fsm_state != BGP_FSM_STATE_ESTABLISHED || !sess->pri_conn)
            {
                continue;
            }
            bgp_update_group_refresh_session_af(sess, (uint16_t)inst->afi, (uint8_t)inst->safi);
            if (BIT_TEST(sess->negotiated_caps, BGP_SESS_CAP_ROUTE_REFRESH))
            {
                (void)bgp_pkt_send_route_refresh(sess->pri_conn, (uint16_t)inst->afi, (uint8_t)inst->safi);
            }
        }
    }

    apply->rc = BGP_APPLY_RC_OK;
}

/* ============================================================================
 * neighbor reflect-client / no neighbor reflect-client（RFC 4456，AF 视图）
 * ========================================================================== */

void bgp_cfg_apply_reflect_client(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local->protocol;
    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    bgp_vrf_t *vrf = bgp_cfg_lookup_vrf(proto, apply, NULL);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }

    bgp_instance_t *inst = (bgp_instance_t *)g_hash_table_lookup(
        vrf->inst_hash, bgp_inst_hash_key(apply->u.reflect_client.afi, apply->u.reflect_client.safi));
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Address family not enabled.");
        return;
    }

    bgp_peer_t *peer = (bgp_peer_t *)g_hash_table_lookup(inst->peer_hash, &apply->u.reflect_client.addr);
    if (!peer)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Neighbor not enabled in this address family.");
        return;
    }

    bool want = !apply->isNo;
    bool cur = BIT_TEST(peer->flags, BGP_PEER_FLAG_RR_CLIENT);
    if (cur == want)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    /* 切换 client 属性会改变 UG-key 中的 RR_CLIENT 标记，需要先离开旧子组、
     * 修改标记、再重新加入正确的子组，然后触发一次 catchup 重发已有 ARO。 */
    bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.reflect_client.addr);

    /* client → non-client 过渡：peer 离开后 RR-client 子组若 peer_count 归零会被销毁，
     * ARO 跟着丢失，下游不会收到 WITHDRAW。这里在 leave 之前把 ARO 内容定向 WITHDRAW
     * 发给该 peer，让对端及时清掉这些路径（split-horizon 接管后本就不该再有）。 */
    if (cur && !want && sess && sess->pri_conn && peer->subgroups)
    {
        for (GList *sl = peer->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (!sg || !sg->adj_rib_out || bgp_adj_rib_out_count(sg->adj_rib_out) == 0u)
            {
                continue;
            }
            GPtrArray *nlri_ptrs = g_ptr_array_new_with_free_func(g_free);
            GHashTableIter aro_iter;
            gpointer aro_key = NULL;
            gpointer aro_val = NULL;
            g_hash_table_iter_init(&aro_iter, sg->adj_rib_out->table);
            while (g_hash_table_iter_next(&aro_iter, &aro_key, &aro_val))
            {
                bgp_nlri_entry_t *copy = (bgp_nlri_entry_t *)g_malloc(sizeof(*copy));
                memcpy(copy, aro_key, sizeof(*copy));
                g_ptr_array_add(nlri_ptrs, copy);
            }
            if (nlri_ptrs->len > 0)
            {
                bgp_send_packed_withdraws_to_session(sess, (uint16_t)inst->afi, (uint8_t)inst->safi,
                                                     (const bgp_nlri_entry_t *const *)nlri_ptrs->pdata,
                                                     (int)nlri_ptrs->len);
            }
            g_ptr_array_free(nlri_ptrs, TRUE);
        }
    }

    if (peer->subgroups)
    {
        bgp_subgroup_peer_leave(peer, sess);
    }
    if (want)
    {
        BIT_SET(peer->flags, BGP_PEER_FLAG_RR_CLIENT);
    }
    else
    {
        BIT_CLR(peer->flags, BGP_PEER_FLAG_RR_CLIENT);
    }
    if (sess && sess->fsm_state == BGP_FSM_STATE_ESTABLISHED && peer->state == BGP_PEER_STATE_ESTABLISHED)
    {
        bgp_update_group_catchup_session(sess);
    }
    apply->rc = BGP_APPLY_RC_OK;
}
