/**
 * @file   isis_route_sync.c
 * @brief  ISIS 路由学习/撤销与接口事件驱动重算
 * @author jhb
 * @date   2026/04/12
 */
#include "isis_route_sync.h"

#include <string.h>

#include "errcode.h"
#include "if_api.h"
#include "if_event.h"
#include "isis.h"
#include "isis_main.h"
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
    entry->vrf_id = ROUTE_VRF_DEFAULT;
    entry->afi = state->afi;
    entry->safi = ROUTE_SAFI_UNICAST;
    entry->prefix_len = state->prefix_len;
    entry->protocol = ROUTE_PROTOCOL_ISIS;
    entry->metric = (int32_t)state->metric;
    entry->preference = ROUTE_ADMIN_DIST_ISIS;
    entry->is_withdraw = 0;
    entry->out_ifindex = state->out_ifindex;
    entry->iter_out_ifindex = state->out_ifindex;
    entry->prefix_addr = state->prefix_addr;
    entry->source_addr = state->source_addr;
    if ((state->nexthop_addr.family == AF_INET || state->nexthop_addr.family == AF_INET6) &&
        !net_addr_is_zero(&state->nexthop_addr))
    {
        entry->nexthop_addr = state->nexthop_addr;
        entry->iter_nexthop_addr = state->nexthop_addr;
    }
    else
    {
        isis_make_zero_addr(state->source_addr.family, &entry->nexthop_addr);
        isis_make_zero_addr(state->source_addr.family, &entry->iter_nexthop_addr);
    }
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

static int isis_route_state_same(const isis_route_state_t *a, const isis_route_state_t *b)
{
    if (!a || !b)
    {
        return 0;
    }

    return (a->afi == b->afi && a->prefix_len == b->prefix_len && a->out_ifindex == b->out_ifindex &&
            a->metric == b->metric && net_addr_equal(&a->prefix_addr, &b->prefix_addr) &&
            net_addr_equal(&a->source_addr, &b->source_addr) && net_addr_equal(&a->nexthop_addr, &b->nexthop_addr))
               ? 1
               : 0;
}

static int isis_build_desired_route_state(const if_api_cache_entry_t *if_entry, uint16_t afi, uint32_t metric,
                                          isis_route_state_t *out)
{
    if (!if_entry || !out || !if_entry->admin_up || if_entry->ifindex == 0u)
    {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->afi = afi;
    out->out_ifindex = if_entry->ifindex;
    out->metric = (metric == 0u) ? ISIS_DEFAULT_IF_METRIC : metric;

    if (afi == ROUTE_AFI_IPV4)
    {
        if (if_entry->ipv4_addr.family != AF_INET || if_entry->ipv4_prefix_len > 32u)
        {
            return 0;
        }
        out->source_addr = if_entry->ipv4_addr;
        isis_make_zero_addr(AF_INET, &out->nexthop_addr);
        out->prefix_addr = if_entry->ipv4_addr;
        out->prefix_len = if_entry->ipv4_prefix_len;
        if (net_addr_prefix_normalize(&out->prefix_addr, out->prefix_len) != 0)
        {
            return 0;
        }
        return 1;
    }

    if (afi == ROUTE_AFI_IPV6)
    {
        if (if_entry->ipv6_addr.family != AF_INET6 || if_entry->ipv6_prefix_len > 128u)
        {
            return 0;
        }
        out->source_addr = if_entry->ipv6_addr;
        isis_make_zero_addr(AF_INET6, &out->nexthop_addr);
        out->prefix_addr = if_entry->ipv6_addr;
        out->prefix_len = if_entry->ipv6_prefix_len;
        if (net_addr_prefix_normalize(&out->prefix_addr, out->prefix_len) != 0)
        {
            return 0;
        }
        return 1;
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
        has_desired = isis_build_desired_route_state(if_entry, afi, af_cfg->metric, &desired);
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

    if (current && isis_route_state_same(current, &desired))
    {
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
        return;
    }
    *next = desired;

    if (isis_route_sync_publish_add(next) != ERRCODE_SUCCESS)
    {
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

    if (inst->learned_routes)
    {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, inst->learned_routes);
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
        if (addr_evt->event == IF_EVENT_ADDR_ADD || addr_evt->event == IF_EVENT_ADDR_DEL)
        {
            g_strlcpy(ifname_out, addr_evt->logical_name, out_sz);
            return (ifname_out[0] != '\0') ? ifname_out : NULL;
        }
    }

    if (msg->payload_len >= sizeof(if_event_msg_t))
    {
        const if_event_msg_t *evt = (const if_event_msg_t *)msg->payload;
        if (evt->event == IF_EVENT_UP || evt->event == IF_EVENT_DOWN)
        {
            g_strlcpy(ifname_out, evt->logical_name, out_sz);
            return (ifname_out[0] != '\0') ? ifname_out : NULL;
        }
    }

    return NULL;
}
