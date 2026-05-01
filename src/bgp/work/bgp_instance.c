/**
 * @file   bgp_instance.c
 * @brief  BGP 地址族实例生命周期实现
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_instance.h"

#include <glib.h>

#include "bgp_calc.h"
#include "bgp_protocol.h"
#include "bgp_rd.h"
#include "bgp_rib.h"
#include "bgp_route_flush.h"
#include "bgp_update_group.h"
#include "bgp_worker.h"
#include "log.h"
#include "net_addr.h"

bgp_instance_t *bgp_instance_create(bgp_afi_t afi, bgp_safi_t safi, bgp_vrf_t *vrf)
{
    bgp_instance_t *inst = g_malloc0(sizeof(bgp_instance_t));
    inst->afi = afi;
    inst->safi = safi;
    inst->vrf = vrf;
    /* QP 地址族默认启用"下一跳保持不变"策略：仍沿用常规 update-group 划分，仅导出时固定 PASS。 */
    if (safi == BGP_SAFI_QP)
    {
        inst->flags |= BGP_INST_FLAG_NH_UNCHANGED;
    }
    /* key: net_addr_t*（堆分配，g_free 释放），value: bgp_peer_t*（负责销毁） */
    inst->peer_hash =
        g_hash_table_new_full(net_addr_hash, net_addr_hash_equal, g_free, (GDestroyNotify)bgp_peer_destroy);
    /* rd_entries: key 指向 entry->key.rd（8 字节 RD），value 借用，所有权在 protocol->rd_hash */
    inst->rd_entries = g_hash_table_new(bgp_rd_hash, bgp_rd_equal);
    inst->calc_queue = bgp_calc_queue_create();
    inst->route_flush_queue = bgp_route_flush_queue_create();

    /* 注入公网（rd=0）entry：所有 AF 都需要至少一条 entry 提供 RIB */
    bgp_protocol_t *proto = g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
    if (proto)
    {
        (void)bgp_protocol_ensure_rd_entry(proto, inst, &BGP_RD_PUBLIC);
    }
    return inst;
}

void bgp_instance_destroy(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }
    bgp_calc_queue_destroy(inst->calc_queue, inst);
    inst->calc_queue = NULL;
    bgp_route_flush_queue_destroy(inst->route_flush_queue, inst);
    inst->route_flush_queue = NULL;
    if (inst->update_groups)
    {
        for (GList *l = inst->update_groups; l; l = l->next)
        {
            bgp_update_group_destroy((bgp_update_group_t *)l->data);
        }
        g_list_free(inst->update_groups);
        inst->update_groups = NULL;
    }
    if (inst->peer_hash)
    {
        g_hash_table_destroy(inst->peer_hash);
        inst->peer_hash = NULL;
    }
    /* 从 protocol->rd_hash 摘除本实例名下所有 RD entry（同步销毁 RIB） */
    bgp_protocol_t *proto = g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
    if (proto)
    {
        bgp_protocol_remove_all_inst_rd_entries(proto, inst);
    }
    if (inst->rd_entries)
    {
        g_hash_table_destroy(inst->rd_entries);
        inst->rd_entries = NULL;
    }
    if (inst->qp_routes)
    {
        g_list_free_full(inst->qp_routes, g_free);
        inst->qp_routes = NULL;
    }
    g_free(inst);
}

bgp_rib_t *bgp_inst_public_rib(bgp_instance_t *inst)
{
    if (!inst || !inst->rd_entries)
    {
        return NULL;
    }
    bgp_rd_entry_t *e = g_hash_table_lookup(inst->rd_entries, &BGP_RD_PUBLIC);
    return e ? e->rib : NULL;
}

bgp_rib_t *bgp_inst_rib_for_nlri(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !inst->rd_entries)
    {
        return NULL;
    }
    if (!bgp_safi_is_vpn(inst->safi))
    {
        return bgp_inst_public_rib(inst);
    }
    bgp_rd_t rd;
    bgp_nlri_extract_rd(nlri, &rd);
    bgp_rd_entry_t *e = g_hash_table_lookup(inst->rd_entries, &rd);
    return e ? e->rib : NULL;
}

bgp_rib_t *bgp_inst_rib_ensure_for_nlri(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !inst->rd_entries)
    {
        return NULL;
    }
    if (!bgp_safi_is_vpn(inst->safi))
    {
        return bgp_inst_public_rib(inst);
    }
    bgp_rd_t rd;
    bgp_nlri_extract_rd(nlri, &rd);
    bgp_rd_entry_t *e = g_hash_table_lookup(inst->rd_entries, &rd);
    if (!e)
    {
        bgp_protocol_t *proto = g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
        if (!proto)
        {
            return NULL;
        }
        e = bgp_protocol_ensure_rd_entry(proto, inst, &rd);
    }
    return e ? e->rib : NULL;
}

typedef struct
{
    bgp_instance_t *inst;
    bgp_inst_rib_iter_cb cb;
    gpointer user_data;
} inst_rib_iter_ctx_t;

static void inst_rib_iter_each(gpointer key, gpointer val, gpointer user_data)
{
    (void)key;
    inst_rib_iter_ctx_t *ctx = (inst_rib_iter_ctx_t *)user_data;
    bgp_rd_entry_t *e = (bgp_rd_entry_t *)val;
    if (!e)
    {
        return;
    }
    ctx->cb(ctx->inst, e, e->rib, ctx->user_data);
}

void bgp_inst_foreach_rib(bgp_instance_t *inst, bgp_inst_rib_iter_cb cb, gpointer user_data)
{
    if (!inst || !inst->rd_entries || !cb)
    {
        return;
    }
    inst_rib_iter_ctx_t ctx = {inst, cb, user_data};
    g_hash_table_foreach(inst->rd_entries, inst_rib_iter_each, &ctx);
}

void bgp_instance_drain_pending(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    for (;;)
    {
        int processed = 0;
        processed += bgp_calc_process_pending(inst);
        processed += bgp_route_flush_process_pending(inst);
        processed += bgp_update_group_process_pending(inst);
        if (processed <= 0)
        {
            break;
        }
    }
}
