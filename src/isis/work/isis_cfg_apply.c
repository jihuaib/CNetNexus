/**
 * @file   isis_cfg_apply.c
 * @brief  ISIS 配置内存态应用实现（CLI / DB 恢复共用）
 *
 * 每个函数负责：参数校验、同配置短路（设 NOOP）、内存更新、副作用，
 * 最终将结果写入 apply->rc 和 apply->errmsg。
 *
 * @author jhb
 * @date   2026/05/16
 */
#include "isis_cfg_apply.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "isis.h"
#include "isis_neighbor.h"
#include "isis_route_sync.h"
#include "isis_worker.h"

static void apply_fail(isis_apply_cmd_t *apply, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void apply_fail(isis_apply_cmd_t *apply, const char *fmt, ...)
{
    apply->rc = ISIS_APPLY_RC_FAIL;
    if (fmt)
    {
        va_list ap;
        va_start(ap, fmt);
        g_vsnprintf(apply->errmsg, sizeof(apply->errmsg), fmt, ap);
        va_end(ap);
    }
}

void isis_cfg_apply_instance_set(isis_apply_cmd_t *apply)
{
    uint32_t tag = apply->u.instance_set.tag;
    if (tag == 0u)
    {
        apply_fail(apply, "ISIS Error: Invalid instance tag");
        return;
    }

    /* 实例已存在时：再次输入 `isis <tag>` 仅用于进入视图，不应覆盖 net/is_type 等字段。
     * 这些字段由各自的 NET_SET / IS_TYPE_SET / AF_SET 路径维护。 */
    isis_instance_cfg_t *inst = isis_lookup_instance(tag);
    if (inst)
    {
        if (inst->vrf_id != apply->u.instance_set.vrf_id || strcmp(inst->vrf_name, apply->u.instance_set.vrf_name) != 0)
        {
            apply_fail(apply, "ISIS Error: process is already bound to VRF %s", inst->vrf_name);
            return;
        }
        apply->rc = ISIS_APPLY_RC_NOOP;
        return;
    }

    inst = isis_get_or_create_instance(tag);
    if (!inst)
    {
        apply_fail(apply, "ISIS Error: Failed to allocate instance %u", tag);
        return;
    }

    g_strlcpy(inst->net, apply->u.instance_set.net, sizeof(inst->net));
    inst->vrf_id = apply->u.instance_set.vrf_id;
    g_strlcpy(inst->vrf_name, apply->u.instance_set.vrf_name, sizeof(inst->vrf_name));
    inst->is_type = apply->u.instance_set.is_type;
    inst->admin_up = apply->u.instance_set.admin_up ? 1u : 0u;
    isis_neighbor_reconcile_instance(inst);
    isis_route_sync_reconcile_instance_all_if(inst);
    apply->rc = ISIS_APPLY_RC_OK;
}

void isis_cfg_apply_instance_del(isis_apply_cmd_t *apply)
{
    uint32_t tag = apply->u.instance_del.tag;
    isis_instance_cfg_t *inst = isis_lookup_instance(tag);
    if (!inst)
    {
        apply->rc = ISIS_APPLY_RC_NOOP;
        return;
    }
    isis_route_sync_withdraw_all_instance_routes(inst);
    if (!g_hash_table_remove(g_isis_work_local->instances, GUINT_TO_POINTER(tag)))
    {
        apply_fail(apply, "ISIS Error: Failed to remove instance %u", tag);
        return;
    }
    apply->rc = ISIS_APPLY_RC_OK;
}

void isis_cfg_apply_net_set(isis_apply_cmd_t *apply)
{
    isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.net_set.tag);
    if (!inst)
    {
        apply_fail(apply, "ISIS Error: Instance %u not found", apply->u.net_set.tag);
        return;
    }
    if (g_strcmp0(inst->net, apply->u.net_set.net) == 0)
    {
        apply->rc = ISIS_APPLY_RC_NOOP;
        return;
    }
    g_strlcpy(inst->net, apply->u.net_set.net, sizeof(inst->net));
    apply->rc = ISIS_APPLY_RC_OK;
}

void isis_cfg_apply_is_type_set(isis_apply_cmd_t *apply)
{
    uint8_t is_type = apply->u.is_type_set.is_type;
    if (is_type != ISIS_IS_TYPE_LEVEL_1 && is_type != ISIS_IS_TYPE_LEVEL_2 && is_type != ISIS_IS_TYPE_LEVEL_1_2)
    {
        apply_fail(apply, "ISIS Error: Invalid is-type %u", is_type);
        return;
    }
    isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.is_type_set.tag);
    if (!inst)
    {
        apply_fail(apply, "ISIS Error: Instance %u not found", apply->u.is_type_set.tag);
        return;
    }
    if (inst->is_type == is_type)
    {
        apply->rc = ISIS_APPLY_RC_NOOP;
        return;
    }
    inst->is_type = is_type;
    isis_neighbor_reconcile_instance(inst);
    apply->rc = ISIS_APPLY_RC_OK;
}

void isis_cfg_apply_af_set(isis_apply_cmd_t *apply)
{
    isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.af_set.tag);
    if (!inst)
    {
        apply_fail(apply, "ISIS Error: Instance %u not found", apply->u.af_set.tag);
        return;
    }
    uint16_t afi = apply->u.af_set.afi;
    if (afi != ISIS_AFI_IPV4 && afi != ISIS_AFI_IPV6)
    {
        apply_fail(apply, "ISIS Error: Invalid AF %u", afi);
        return;
    }

    /* cost-style narrow 不支持 IPv6 */
    if (afi == ISIS_AFI_IPV6 && inst->cost_style == ISIS_COST_STYLE_NARROW)
    {
        apply_fail(apply, "ISIS Error: IPv6 AF requires 'cost-style wide' (narrow does not support IPv6)");
        return;
    }

    uint8_t *flag = (afi == ISIS_AFI_IPV4) ? &inst->af_ipv4 : &inst->af_ipv6;
    if (*flag == 1u)
    {
        apply->rc = ISIS_APPLY_RC_NOOP;
        return;
    }
    *flag = 1u;
    isis_neighbor_reconcile_instance(inst);
    isis_route_sync_reconcile_instance_all_if(inst);
    apply->rc = ISIS_APPLY_RC_OK;
}

void isis_cfg_apply_af_del(isis_apply_cmd_t *apply)
{
    isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.af_del.tag);
    if (!inst)
    {
        apply_fail(apply, "ISIS Error: Instance %u not found", apply->u.af_del.tag);
        return;
    }
    uint16_t afi = apply->u.af_del.afi;
    if (afi != ISIS_AFI_IPV4 && afi != ISIS_AFI_IPV6)
    {
        apply_fail(apply, "ISIS Error: Invalid AF %u", afi);
        return;
    }
    uint8_t *flag = (afi == ISIS_AFI_IPV4) ? &inst->af_ipv4 : &inst->af_ipv6;
    if (*flag == 0u)
    {
        apply->rc = ISIS_APPLY_RC_NOOP;
        return;
    }
    *flag = 0u;
    isis_neighbor_reconcile_instance(inst);
    isis_route_sync_reconcile_instance_all_if(inst);
    apply->rc = ISIS_APPLY_RC_OK;
}

void isis_cfg_apply_if_set(isis_apply_cmd_t *apply)
{
    isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.if_set.tag);
    if (!inst)
    {
        apply_fail(apply, "ISIS Error: Instance %u not found", apply->u.if_set.tag);
        return;
    }
    if (apply->u.if_set.cfg.ifname[0] == '\0')
    {
        apply_fail(apply, "ISIS Error: Missing interface name");
        return;
    }
    const if_api_cache_entry_t *if_entry =
        isis_if_cfg_any_enabled(&apply->u.if_set.cfg) ? if_api_cache_lookup(apply->u.if_set.cfg.ifname) : NULL;
    if (if_entry && !isis_if_entry_matches_instance(inst, if_entry))
    {
        apply_fail(apply, "ISIS Error: interface %s is not in VRF %s", apply->u.if_set.cfg.ifname, inst->vrf_name);
        return;
    }

    /* cost-style 校验：narrow 模式下 metric 不能超过 63；且 narrow 不支持 IPv6 接口 */
    if (inst->cost_style == ISIS_COST_STYLE_NARROW)
    {
        const isis_if_cfg_t *new_cfg = &apply->u.if_set.cfg;
        if (new_cfg->v4.enabled && new_cfg->v4.metric > ISIS_NARROW_MAX_METRIC)
        {
            apply_fail(apply, "ISIS Error: metric %u exceeds narrow max %u; configure 'cost-style wide' first",
                       new_cfg->v4.metric, ISIS_NARROW_MAX_METRIC);
            return;
        }
        if (new_cfg->v6.enabled)
        {
            apply_fail(apply, "ISIS Error: IPv6 interface not supported under 'cost-style narrow'");
            return;
        }
    }

    if (!isis_if_cfg_any_enabled(&apply->u.if_set.cfg))
    {
        (void)g_hash_table_remove(inst->if_cfgs, apply->u.if_set.cfg.ifname);
        isis_neighbor_on_if_removed(inst, apply->u.if_set.cfg.ifname);
        isis_route_sync_reconcile_instance_if(inst, apply->u.if_set.cfg.ifname);
        apply->rc = ISIS_APPLY_RC_OK;
        return;
    }

    isis_if_cfg_t *cfg = (isis_if_cfg_t *)g_hash_table_lookup(inst->if_cfgs, apply->u.if_set.cfg.ifname);
    uint64_t last_hello_tx_msec = 0u;
    if (cfg)
    {
        last_hello_tx_msec = cfg->last_hello_tx_msec;
        *cfg = apply->u.if_set.cfg;
        cfg->last_hello_tx_msec = last_hello_tx_msec;
    }
    else
    {
        cfg = g_malloc0(sizeof(*cfg));
        if (!cfg)
        {
            apply_fail(apply, "ISIS Error: Failed to allocate interface config");
            return;
        }
        *cfg = apply->u.if_set.cfg;
        g_hash_table_insert(inst->if_cfgs, g_strdup(cfg->ifname), cfg);
    }

    isis_neighbor_reconcile_if(cfg->ifname);
    isis_route_sync_reconcile_instance_if(inst, cfg->ifname);
    apply->rc = ISIS_APPLY_RC_OK;
}

void isis_cfg_apply_if_del(isis_apply_cmd_t *apply)
{
    isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.if_del.tag);
    if (!inst)
    {
        apply_fail(apply, "ISIS Error: Instance %u not found", apply->u.if_del.tag);
        return;
    }
    if (apply->u.if_del.ifname[0] == '\0')
    {
        apply_fail(apply, "ISIS Error: Missing interface name");
        return;
    }
    if (!g_hash_table_remove(inst->if_cfgs, apply->u.if_del.ifname))
    {
        apply->rc = ISIS_APPLY_RC_NOOP;
        return;
    }
    isis_neighbor_on_if_removed(inst, apply->u.if_del.ifname);
    isis_route_sync_reconcile_instance_if(inst, apply->u.if_del.ifname);
    apply->rc = ISIS_APPLY_RC_OK;
}

void isis_cfg_apply_cost_style_set(isis_apply_cmd_t *apply)
{
    uint8_t cs = apply->u.cost_style_set.cost_style;
    if (cs != ISIS_COST_STYLE_NARROW && cs != ISIS_COST_STYLE_WIDE)
    {
        apply_fail(apply, "ISIS Error: Invalid cost-style %u", cs);
        return;
    }
    isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.cost_style_set.tag);
    if (!inst)
    {
        apply_fail(apply, "ISIS Error: Instance %u not found", apply->u.cost_style_set.tag);
        return;
    }
    if (inst->cost_style == cs)
    {
        apply->rc = ISIS_APPLY_RC_NOOP;
        return;
    }

    /* narrow→wide：随时可切。
     * wide→narrow：检查现有接口 metric 是否超过 63，以及是否存在已启用的 IPv6 AF/接口 */
    if (cs == ISIS_COST_STYLE_NARROW)
    {
        if (inst->af_ipv6)
        {
            apply_fail(
                apply,
                "ISIS Error: 'cost-style narrow' requires disabling 'af ipv6' first (narrow doesn't support IPv6)");
            return;
        }
        if (inst->if_cfgs)
        {
            GHashTableIter iter;
            gpointer k = NULL;
            gpointer v = NULL;
            g_hash_table_iter_init(&iter, inst->if_cfgs);
            while (g_hash_table_iter_next(&iter, &k, &v))
            {
                const isis_if_cfg_t *icfg = (const isis_if_cfg_t *)v;
                if (!icfg)
                {
                    continue;
                }
                if (icfg->v4.enabled && icfg->v4.metric > ISIS_NARROW_MAX_METRIC)
                {
                    apply_fail(apply, "ISIS Error: interface %s ipv4 metric %u exceeds narrow max %u; lower it first",
                               icfg->ifname, icfg->v4.metric, ISIS_NARROW_MAX_METRIC);
                    return;
                }
                if (icfg->v6.enabled)
                {
                    apply_fail(apply,
                               "ISIS Error: interface %s has IPv6 enabled; disable IPv6 before switching to narrow",
                               icfg->ifname);
                    return;
                }
            }
        }
    }

    inst->cost_style = cs;

    /* 厂商语义：cost-style 切换等价于重启 ISIS 进程
     *   1) 撤销已学路由（清空 RIB 占用）
     *   2) 清空邻居表，下个 IIH 触发重新建邻；对端 hold timeout 后会清掉旧 LSP
     *   3) 清空本地 LSDB，避免按新风格解析时残留旧 TLV
     *   4) 重置 LSP TX 节流并 bump seq，让新风格 LSP 在下一个 tick 立刻发出去 */
    isis_route_sync_withdraw_all_instance_routes(inst);
    if (inst->neighbors)
    {
        g_hash_table_remove_all(inst->neighbors);
    }
    if (inst->lsdb_entries)
    {
        g_hash_table_remove_all(inst->lsdb_entries);
    }
    inst->last_lsp_tx_msec = 0u;
    inst->lsp_seq_l1 += 1u;
    inst->lsp_seq_l2 += 1u;

    apply->rc = ISIS_APPLY_RC_OK;
}
