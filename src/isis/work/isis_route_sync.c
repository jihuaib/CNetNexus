/**
 * @file   isis_route_sync.c
 * @brief  ISIS 路由学习/撤销与接口事件驱动重算
 * @author jhb
 * @date   2026/04/12
 */
#include "isis_route_sync.h"

#include <string.h>

#include "errcode.h"
#include "if.h"
#include "isis.h"
#include "isis_main.h"
#include "isis_nexthop.h"
#include "isis_route.h"
#include "log.h"
#include "route.h"

#define ISIS_ROUTE_SYNC_TIMEOUT_MS 3000u
#define ISIS_ROUTE_KEY_MAX (IF_LOGICAL_NAME_MAX + 24u)

static void isis_route_key_format(char *buf, size_t sz, const char *ifname, uint16_t afi)
{
    if (!buf || sz == 0)
    {
        return;
    }
    g_snprintf(buf, sz, "%s|%u", ifname ? ifname : "", (unsigned)afi);
}

static void isis_make_zero_addr(sa_family_t family, net_addr_t *out)
{
    if (!out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->family = family;
}

static const isis_if_af_cfg_t *isis_route_sync_get_af_cfg(const isis_instance_cfg_t *inst, const isis_if_cfg_t *if_cfg,
                                                          uint16_t afi)
{
    if (!inst || !if_cfg)
    {
        return NULL;
    }

    const isis_if_af_cfg_t *af_cfg = isis_if_cfg_af_const(if_cfg, afi);
    if (!af_cfg || !af_cfg->enabled)
    {
        return NULL;
    }

    if ((afi == ROUTE_AFI_IPV4 && !inst->af_ipv4) || (afi == ROUTE_AFI_IPV6 && !inst->af_ipv6))
    {
        return NULL;
    }

    return af_cfg;
}

static void isis_fill_route_msg_entry(const isis_route_state_t *state, route_msg_entry_t *entry)
{
    if (!state || !entry)
    {
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->vrf_id = state->vrf_id;
    entry->afi = state->afi;
    entry->safi = ROUTE_SAFI_UNICAST;
    entry->prefix_len = state->prefix_len;
    entry->protocol = ROUTE_PROTOCOL_ISIS;
    entry->metric = (int32_t)state->metric;
    entry->preference = ROUTE_ADMIN_DIST_ISIS;
    entry->is_withdraw = 0;
    (void)isis_route_state_get_out_ifindex(state, &entry->out_ifindex);
    entry->prefix_addr = state->prefix_addr;
    entry->source_addr = state->source_addr;
    entry->nh_type = ROUTE_NH_TYPE_IP;
    entry->nexthop_id = state->nexthop_id;
}

int isis_route_sync_publish_add(const isis_route_state_t *state)
{
    route_msg_entry_t entry;
    isis_fill_route_msg_entry(state, &entry);
    return route_rpc_add_wait(isis_local_ipc_ctx(), &entry, ISIS_ROUTE_SYNC_TIMEOUT_MS);
}

int isis_route_sync_publish_del(const isis_route_state_t *state)
{
    route_msg_entry_t entry;
    isis_fill_route_msg_entry(state, &entry);
    return route_rpc_del_wait(isis_local_ipc_ctx(), &entry, ISIS_ROUTE_SYNC_TIMEOUT_MS);
}

static int isis_route_sync_state_same(const isis_route_state_t *a, const isis_route_state_t *b)
{
    if (!a || !b)
    {
        return 0;
    }

    return isis_route_state_same(a, b);
}

static int isis_build_desired_route_state(isis_instance_cfg_t *inst, const if_api_cache_entry_t *if_entry, uint16_t afi,
                                          uint32_t metric, isis_route_state_t *out)
{
    if (!inst || !if_entry || !out || !isis_if_entry_matches_instance(inst, if_entry) || !if_entry->proto_up ||
        if_entry->ifindex == 0u)
    {
        return 0;
    }

    isis_nexthop_table_t *nh_table = isis_instance_nexthop_table(inst, afi);
    if (!nh_table)
    {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->vrf_id = inst->vrf_id;
    out->afi = afi;
    out->metric = (metric == 0u) ? ISIS_DEFAULT_IF_METRIC : metric;

    if (afi == ROUTE_AFI_IPV4)
    {
        if (if_entry->ipv4_addr.family != AF_INET || if_entry->ipv4_prefix_len > 32u)
        {
            return 0;
        }
        out->source_addr = if_entry->ipv4_addr;
        out->prefix_addr = if_entry->ipv4_addr;
        out->prefix_len = if_entry->ipv4_prefix_len;
        if (net_addr_prefix_normalize(&out->prefix_addr, out->prefix_len) != 0)
        {
            return 0;
        }
        net_addr_t zero_nh;
        isis_make_zero_addr(AF_INET, &zero_nh);
        return (isis_route_state_set_nexthop(out, nh_table, if_entry->ifindex, if_entry->ifindex, &zero_nh) ==
                ERRCODE_SUCCESS)
                   ? 1
                   : 0;
    }

    if (afi == ROUTE_AFI_IPV6)
    {
        if (if_entry->ipv6_addr.family != AF_INET6 || if_entry->ipv6_prefix_len > 128u)
        {
            return 0;
        }
        out->source_addr = if_entry->ipv6_addr;
        out->prefix_addr = if_entry->ipv6_addr;
        out->prefix_len = if_entry->ipv6_prefix_len;
        if (net_addr_prefix_normalize(&out->prefix_addr, out->prefix_len) != 0)
        {
            return 0;
        }
        net_addr_t zero_nh;
        isis_make_zero_addr(AF_INET6, &zero_nh);
        return (isis_route_state_set_nexthop(out, nh_table, if_entry->ifindex, if_entry->ifindex, &zero_nh) ==
                ERRCODE_SUCCESS)
                   ? 1
                   : 0;
    }

    return 0;
}

static void isis_withdraw_instance_if_afi(isis_instance_cfg_t *inst, const char *ifname, uint16_t afi)
{
    if (!inst || !inst->route_states || !ifname || ifname[0] == '\0')
    {
        return;
    }

    char route_key[ISIS_ROUTE_KEY_MAX];
    isis_route_key_format(route_key, sizeof(route_key), ifname, afi);

    isis_route_state_t *state = (isis_route_state_t *)g_hash_table_lookup(inst->route_states, route_key);
    if (state)
    {
        (void)isis_route_sync_publish_del(state);
        (void)g_hash_table_remove(inst->route_states, route_key);
    }
}

static void isis_reconcile_instance_if_afi(isis_instance_cfg_t *inst, const isis_if_cfg_t *if_cfg,
                                           const if_api_cache_entry_t *if_entry, uint16_t afi)
{
    if (!inst || !if_cfg || !if_cfg->ifname[0] || !inst->route_states)
    {
        return;
    }

    char route_key[ISIS_ROUTE_KEY_MAX];
    isis_route_key_format(route_key, sizeof(route_key), if_cfg->ifname, afi);

    isis_route_state_t *current = (isis_route_state_t *)g_hash_table_lookup(inst->route_states, route_key);
    const isis_if_af_cfg_t *af_cfg = isis_route_sync_get_af_cfg(inst, if_cfg, afi);

    isis_route_state_t desired;
    int has_desired = 0;
    if (inst->admin_up && af_cfg)
    {
        has_desired = isis_build_desired_route_state(inst, if_entry, afi, af_cfg->metric, &desired);
    }

    if (!has_desired)
    {
        if (current)
        {
            (void)isis_route_sync_publish_del(current);
            (void)g_hash_table_remove(inst->route_states, route_key);
        }
        return;
    }

    if (current && isis_route_sync_state_same(current, &desired))
    {
        isis_route_state_reset(&desired);
        return;
    }

    if (current)
    {
        (void)isis_route_sync_publish_del(current);
        (void)g_hash_table_remove(inst->route_states, route_key);
    }

    isis_route_state_t *next = g_malloc0(sizeof(*next));
    if (!next)
    {
        isis_route_state_reset(&desired);
        return;
    }
    *next = desired;
    desired.nexthop_id = 0u;

    if (isis_route_sync_publish_add(next) != ERRCODE_SUCCESS)
    {
        isis_route_state_reset(next);
        g_free(next);
        return;
    }

    g_hash_table_replace(inst->route_states, g_strdup(route_key), next);
}

void isis_route_sync_reconcile_instance_if(isis_instance_cfg_t *inst, const char *ifname)
{
    if (!inst || !ifname || ifname[0] == '\0')
    {
        return;
    }

    isis_if_cfg_t *if_cfg = NULL;
    if (inst->if_cfgs)
    {
        if_cfg = (isis_if_cfg_t *)g_hash_table_lookup(inst->if_cfgs, ifname);
    }

    if (!if_cfg)
    {
        isis_withdraw_instance_if_afi(inst, ifname, ROUTE_AFI_IPV4);
        isis_withdraw_instance_if_afi(inst, ifname, ROUTE_AFI_IPV6);
        return;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(ifname);
    isis_reconcile_instance_if_afi(inst, if_cfg, if_entry, ROUTE_AFI_IPV4);
    isis_reconcile_instance_if_afi(inst, if_cfg, if_entry, ROUTE_AFI_IPV6);
}

typedef struct isis_reconcile_if_ctx
{
    isis_instance_cfg_t *inst;
} isis_reconcile_if_ctx_t;

static void isis_reconcile_instance_if_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)value;
    const char *ifname = (const char *)key;
    isis_reconcile_if_ctx_t *ctx = (isis_reconcile_if_ctx_t *)user_data;
    if (!ctx || !ctx->inst || !ifname)
    {
        return;
    }
    isis_route_sync_reconcile_instance_if(ctx->inst, ifname);
}

void isis_route_sync_reconcile_instance_all_if(isis_instance_cfg_t *inst)
{
    if (!inst || !inst->if_cfgs)
    {
        return;
    }

    isis_reconcile_if_ctx_t ctx = {
        .inst = inst,
    };
    g_hash_table_foreach(inst->if_cfgs, isis_reconcile_instance_if_cb, &ctx);
}

void isis_route_sync_withdraw_all_instance_routes(isis_instance_cfg_t *inst)
{
    if (!inst)
    {
        return;
    }

    if (inst->route_states)
    {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, inst->route_states);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            (void)key;
            isis_route_state_t *state = (isis_route_state_t *)value;
            if (state)
            {
                (void)isis_route_sync_publish_del(state);
            }
            g_hash_table_iter_remove(&iter);
        }
    }

    if (inst->learned_route_heads)
    {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, inst->learned_route_heads);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            (void)key;
            const isis_route_head_t *head = (const isis_route_head_t *)value;
            const isis_route_path_t *best = isis_route_head_best_path(head);
            if (best)
            {
                (void)isis_route_sync_publish_del(&best->state);
            }
            g_hash_table_iter_remove(&iter);
        }
    }
}

void isis_route_sync_reconcile_all_instances_if(const char *ifname)
{
    if (!g_isis_work_local || !g_isis_work_local->instances || !ifname || ifname[0] == '\0')
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_isis_work_local->instances);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        isis_route_sync_reconcile_instance_if((isis_instance_cfg_t *)value, ifname);
    }
}

void isis_route_sync_reconcile_all_instances(void)
{
    if (!g_isis_work_local || !g_isis_work_local->instances)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_isis_work_local->instances);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        isis_route_sync_reconcile_instance_all_if((isis_instance_cfg_t *)value);
    }
}

static void isis_route_sync_replay_instance_route_state_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    uint32_t *replayed = (uint32_t *)user_data;
    const isis_route_state_t *state = (const isis_route_state_t *)value;
    if (state && isis_route_sync_publish_add(state) == ERRCODE_SUCCESS && replayed)
    {
        (*replayed)++;
    }
}

static void isis_route_sync_replay_instance_learned_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    uint32_t *replayed = (uint32_t *)user_data;
    const isis_route_head_t *head = (const isis_route_head_t *)value;
    const isis_route_path_t *best = isis_route_head_best_path(head);
    if (best && isis_route_sync_publish_add(&best->state) == ERRCODE_SUCCESS && replayed)
    {
        (*replayed)++;
    }
}

static void isis_route_sync_replay_instance(isis_instance_cfg_t *inst, uint32_t *replayed)
{
    if (!inst)
    {
        return;
    }
    if (inst->route_states)
    {
        g_hash_table_foreach(inst->route_states, isis_route_sync_replay_instance_route_state_cb, replayed);
    }
    if (inst->learned_route_heads)
    {
        g_hash_table_foreach(inst->learned_route_heads, isis_route_sync_replay_instance_learned_cb, replayed);
    }
}

void isis_nexthop_resync_all_instances(void)
{
    if (!g_isis_work_local || !g_isis_work_local->instances)
    {
        return;
    }

    const uint16_t afis[] = {ROUTE_AFI_IPV4, ROUTE_AFI_IPV6};
    uint32_t pushed = 0;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_isis_work_local->instances);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        isis_instance_cfg_t *inst = (isis_instance_cfg_t *)value;
        for (size_t i = 0; i < G_N_ELEMENTS(afis); i++)
        {
            isis_nexthop_table_t *table = isis_instance_nexthop_table(inst, afis[i]);
            if (table)
            {
                pushed += isis_nexthop_table_resync(table);
            }
        }
    }
    if (pushed > 0)
    {
        LOG_INFO("ISIS: re-pushed %u nexthop object(s) to ROUTE after restart", pushed);
    }
}

void isis_route_sync_replay_all_instances(void)
{
    if (!g_isis_work_local || !g_isis_work_local->instances)
    {
        return;
    }

    uint32_t replayed = 0;
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_isis_work_local->instances);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        isis_route_sync_replay_instance((isis_instance_cfg_t *)value, &replayed);
    }
    LOG_INFO("ISIS: replayed %u route(s) to ROUTE", replayed);
}

const char *isis_route_sync_if_event_get_logical_name(const dev_ipc_message_t *msg, char *ifname_out, size_t out_sz)
{
    if (!msg || !msg->payload || !ifname_out || out_sz == 0)
    {
        return NULL;
    }

    ifname_out[0] = '\0';

    if (msg->payload_len >= sizeof(if_addr_event_msg_t))
    {
        const if_addr_event_msg_t *addr_evt = (const if_addr_event_msg_t *)msg->payload;
        if (addr_evt->event == IF_EVENT_PROTO_UP || addr_evt->event == IF_EVENT_PROTO_DOWN)
        {
            g_strlcpy(ifname_out, addr_evt->logical_name, out_sz);
            return (ifname_out[0] != '\0') ? ifname_out : NULL;
        }
    }

    if (msg->payload_len >= sizeof(if_event_msg_t))
    {
        const if_event_msg_t *evt = (const if_event_msg_t *)msg->payload;
        if (evt->event == IF_EVENT_LINK_UP || evt->event == IF_EVENT_LINK_DOWN)
        {
            g_strlcpy(ifname_out, evt->logical_name, out_sz);
            return (ifname_out[0] != '\0') ? ifname_out : NULL;
        }
    }

    return NULL;
}
