/**
 * @file   ospf_worker.c
 * @brief  Single-owner OSPFv2 worker thread
 */
#include "ospf_worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "errcode.h"
#include "log.h"
#include "ospf_lsa.h"
#include "ospf_main.h"
#include "ospf_packet.h"
#include "ospf_route_sync.h"
#include "ospf_show.h"
#include "ospf_spf.h"

#define OSPF_MAX_EPOLL_EVENTS 8

typedef enum ospf_worker_cmd_type
{
    OSPF_WORKER_CMD_SHOW = 1,
    OSPF_WORKER_CMD_IF_EVENT = 2,
    OSPF_WORKER_CMD_APPLY = 3,
    OSPF_WORKER_CMD_ROUTE_READY = 4,
    OSPF_WORKER_CMD_IF_DOWN = 5,
    OSPF_WORKER_CMD_SHUTDOWN = 6,
} ospf_worker_cmd_type_t;

typedef struct ospf_worker_cmd
{
    ospf_worker_cmd_type_t type;
    dev_ipc_message_t *msg;
    ospf_apply_cmd_t *apply;
    int waitable;
    int done;
    int rc;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} ospf_worker_cmd_t;

ospf_work_local_t *g_ospf_work_local;

static int g_ospf_cmd_tag;
static int g_ospf_raw_tag;
static int g_ospf_timer_tag;

uint64_t ospf_now_msec(void)
{
    return (uint64_t)(g_get_monotonic_time() / 1000);
}

gboolean ospf_if_entry_matches_vrf(const char *vrf_name, const if_api_cache_entry_t *entry)
{
    const char *expected = (vrf_name && vrf_name[0] != '\0') ? vrf_name : "public";
    const char *actual = (entry && entry->vrf_name[0] != '\0') ? entry->vrf_name : "public";
    return entry && strcmp(expected, actual) == 0;
}

static void ospf_neighbor_free(gpointer data)
{
    ospf_neighbor_t *nbr = (ospf_neighbor_t *)data;
    if (!nbr)
    {
        return;
    }
    if (nbr->request_keys)
    {
        g_ptr_array_free(nbr->request_keys, TRUE);
    }
    if (nbr->dd_summaries)
    {
        g_ptr_array_free(nbr->dd_summaries, TRUE);
    }
    if (nbr->last_dbd_tx)
    {
        g_byte_array_unref(nbr->last_dbd_tx);
    }
    if (nbr->last_dbd_rx)
    {
        g_byte_array_unref(nbr->last_dbd_rx);
    }
    if (nbr->retrans_lsas)
    {
        g_hash_table_destroy(nbr->retrans_lsas);
    }
    g_free(nbr);
}

static void ospf_lsa_entry_free(gpointer data)
{
    ospf_lsa_entry_t *entry = (ospf_lsa_entry_t *)data;
    if (!entry)
    {
        return;
    }
    if (entry->raw)
    {
        g_byte_array_unref(entry->raw);
    }
    g_free(entry);
}

static void ospf_route_free(gpointer data)
{
    g_free(data);
}

void ospf_worker_withdraw_all_routes(ospf_instance_t *inst)
{
    if (!inst || !inst->routes)
    {
        return;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->routes);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_route_t *route = (const ospf_route_t *)value;
        if (route)
        {
            if (ospf_route_sync_enqueue_del(route) != ERRCODE_SUCCESS)
            {
                LOG_WARN("OSPF process %u: failed to queue route withdrawal", inst->process_id);
            }
        }
        g_hash_table_iter_remove(&iter);
    }
}

static void ospf_instance_free(gpointer data)
{
    ospf_instance_t *inst = (ospf_instance_t *)data;
    if (!inst)
    {
        return;
    }

    ospf_packet_reset_instance(inst);
    ospf_worker_withdraw_all_routes(inst);
    if (inst->areas)
    {
        g_hash_table_destroy(inst->areas);
    }
    if (inst->if_cfgs)
    {
        g_hash_table_destroy(inst->if_cfgs);
    }
    if (inst->neighbors)
    {
        g_hash_table_destroy(inst->neighbors);
    }
    if (inst->lsdb)
    {
        g_hash_table_destroy(inst->lsdb);
    }
    if (inst->routes)
    {
        g_hash_table_destroy(inst->routes);
    }
    g_free(inst);
}

static ospf_instance_t *ospf_instance_create(uint32_t process_id)
{
    ospf_instance_t *inst = g_malloc0(sizeof(*inst));
    if (!inst)
    {
        return NULL;
    }

    inst->process_id = process_id;
    inst->admin_up = 1u;
    inst->next_lsa_sequence = OSPF_LSA_INITIAL_SEQUENCE;
    inst->areas = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, NULL);
    inst->if_cfgs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    inst->neighbors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ospf_neighbor_free);
    inst->lsdb = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ospf_lsa_entry_free);
    inst->routes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ospf_route_free);
    if (!inst->areas || !inst->if_cfgs || !inst->neighbors || !inst->lsdb || !inst->routes)
    {
        ospf_instance_free(inst);
        return NULL;
    }
    return inst;
}

ospf_instance_t *ospf_lookup_instance(uint32_t process_id)
{
    if (!g_ospf_work_local || !g_ospf_work_local->instances || process_id == 0u)
    {
        return NULL;
    }
    return (ospf_instance_t *)g_hash_table_lookup(g_ospf_work_local->instances, GUINT_TO_POINTER(process_id));
}

ospf_instance_t *ospf_get_or_create_instance(uint32_t process_id)
{
    ospf_instance_t *inst = ospf_lookup_instance(process_id);
    if (inst)
    {
        return inst;
    }
    if (process_id == 0u)
    {
        return NULL;
    }

    inst = ospf_instance_create(process_id);
    if (inst)
    {
        g_hash_table_insert(g_ospf_work_local->instances, GUINT_TO_POINTER(process_id), inst);
    }
    return inst;
}

gboolean ospf_instance_has_area(const ospf_instance_t *inst, uint32_t area_id)
{
    return inst && inst->areas && g_hash_table_contains(inst->areas, &area_id);
}

static gboolean ospf_instance_add_area(ospf_instance_t *inst, uint32_t area_id)
{
    if (ospf_instance_has_area(inst, area_id))
    {
        return TRUE;
    }

    uint32_t *key = g_new(uint32_t, 1);
    if (!key)
    {
        return FALSE;
    }
    *key = area_id;
    g_hash_table_add(inst->areas, key);
    return TRUE;
}

static ospf_worker_cmd_t *ospf_worker_cmd_create(ospf_worker_cmd_type_t type, dev_ipc_message_t *msg, int waitable)
{
    ospf_worker_cmd_t *cmd = g_malloc0(sizeof(*cmd));
    if (!cmd)
    {
        return NULL;
    }
    cmd->type = type;
    cmd->msg = msg;
    cmd->waitable = waitable;
    if (waitable)
    {
        pthread_mutex_init(&cmd->mutex, NULL);
        pthread_cond_init(&cmd->cond, NULL);
    }
    return cmd;
}

static void ospf_worker_cmd_destroy(ospf_worker_cmd_t *cmd)
{
    if (!cmd)
    {
        return;
    }
    if (cmd->waitable)
    {
        pthread_cond_destroy(&cmd->cond);
        pthread_mutex_destroy(&cmd->mutex);
    }
    g_free(cmd);
}

static void ospf_worker_cmd_complete(ospf_worker_cmd_t *cmd, int rc)
{
    if (!cmd || !cmd->waitable)
    {
        return;
    }
    pthread_mutex_lock(&cmd->mutex);
    cmd->rc = rc;
    cmd->done = 1;
    pthread_cond_signal(&cmd->cond);
    pthread_mutex_unlock(&cmd->mutex);
}

static int ospf_worker_cmd_wait(ospf_worker_cmd_t *cmd)
{
    pthread_mutex_lock(&cmd->mutex);
    while (!cmd->done)
    {
        pthread_cond_wait(&cmd->cond, &cmd->mutex);
    }
    int rc = cmd->rc;
    pthread_mutex_unlock(&cmd->mutex);
    return rc;
}

static void ospf_worker_signal(void)
{
    uint64_t one = 1u;
    if (!g_ospf_work_local || g_ospf_work_local->cmd_eventfd < 0)
    {
        return;
    }
    if (write(g_ospf_work_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one) && errno != EAGAIN &&
        errno != EWOULDBLOCK)
    {
        LOG_PERROR("OSPF: command eventfd write failed");
    }
}

static int ospf_worker_enqueue(ospf_worker_cmd_t *cmd)
{
    if (!cmd || !g_ospf_work_local || !g_ospf_work_local->cmd_queue || g_ospf_work_local->cmd_eventfd < 0)
    {
        return ERRCODE_FAIL;
    }
    g_async_queue_push(g_ospf_work_local->cmd_queue, cmd);
    ospf_worker_signal();
    return ERRCODE_SUCCESS;
}

static void ospf_apply_fail(ospf_apply_cmd_t *apply, const char *text)
{
    apply->rc = OSPF_APPLY_RC_FAIL;
    g_strlcpy(apply->errmsg, text, sizeof(apply->errmsg));
}

static gboolean ospf_router_id_in_use(uint32_t router_id, uint32_t except_process_id)
{
    if (router_id == 0u || !g_ospf_work_local || !g_ospf_work_local->instances)
    {
        return FALSE;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_ospf_work_local->instances);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_instance_t *inst = (const ospf_instance_t *)value;
        if (inst->process_id != except_process_id && inst->router_id == router_id)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static uint32_t ospf_interface_owner(const char *ifname, uint32_t except_process_id)
{
    if (!ifname || !g_ospf_work_local || !g_ospf_work_local->instances)
    {
        return 0u;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_ospf_work_local->instances);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_instance_t *inst = (const ospf_instance_t *)value;
        const ospf_if_cfg_t *cfg = g_hash_table_lookup(inst->if_cfgs, ifname);
        if (inst->process_id != except_process_id && cfg && cfg->enabled)
        {
            return inst->process_id;
        }
    }
    return 0u;
}

static int ospf_if_config_equal(const ospf_if_cfg_t *a, const ospf_if_cfg_t *b)
{
    return a && b && a->enabled == b->enabled && a->passive == b->passive && a->network_type == b->network_type &&
           a->priority == b->priority && a->cost == b->cost && a->hello_interval == b->hello_interval &&
           a->dead_interval == b->dead_interval && a->area_id == b->area_id;
}

static void ospf_apply_instance_set(ospf_apply_cmd_t *apply)
{
    uint32_t process_id = apply->u.instance_set.process_id;
    if (process_id == 0u)
    {
        ospf_apply_fail(apply, "OSPF Error: invalid process ID");
        return;
    }

    ospf_instance_t *inst = ospf_lookup_instance(process_id);
    if (inst)
    {
        if (inst->vrf_id != apply->u.instance_set.vrf_id || strcmp(inst->vrf_name, apply->u.instance_set.vrf_name) != 0)
        {
            apply->rc = OSPF_APPLY_RC_FAIL;
            g_snprintf(apply->errmsg, sizeof(apply->errmsg), "OSPF Error: process is already bound to VRF %s",
                       inst->vrf_name);
            return;
        }
        if (apply->u.instance_set.router_id == 0u || apply->u.instance_set.router_id == inst->router_id)
        {
            apply->rc = OSPF_APPLY_RC_NOOP;
            return;
        }
        if (ospf_router_id_in_use(apply->u.instance_set.router_id, process_id))
        {
            ospf_apply_fail(apply, "OSPF Error: router ID is already used by another process");
            return;
        }
        ospf_packet_reset_instance(inst);
        ospf_worker_withdraw_all_routes(inst);
        g_hash_table_remove_all(inst->lsdb);
        inst->router_id = apply->u.instance_set.router_id;
        inst->next_lsa_sequence = OSPF_LSA_INITIAL_SEQUENCE;
        inst->next_lsa_originate_msec = 0u;
        ospf_worker_reconcile_all();
        apply->rc = OSPF_APPLY_RC_OK;
        return;
    }

    if (ospf_router_id_in_use(apply->u.instance_set.router_id, process_id))
    {
        ospf_apply_fail(apply, "OSPF Error: router ID is already used by another process");
        return;
    }

    inst = ospf_get_or_create_instance(process_id);
    if (!inst)
    {
        ospf_apply_fail(apply, "OSPF Error: failed to create process");
        return;
    }
    inst->router_id = apply->u.instance_set.router_id;
    inst->vrf_id = apply->u.instance_set.vrf_id;
    g_strlcpy(inst->vrf_name, apply->u.instance_set.vrf_name, sizeof(inst->vrf_name));
    apply->rc = OSPF_APPLY_RC_OK;
}

static void ospf_apply_instance_del(ospf_apply_cmd_t *apply)
{
    uint32_t process_id = apply->u.instance_del.process_id;
    if (process_id == 0u)
    {
        ospf_apply_fail(apply, "OSPF Error: invalid process ID");
        return;
    }
    if (!ospf_lookup_instance(process_id))
    {
        apply->rc = OSPF_APPLY_RC_NOOP;
        return;
    }
    g_hash_table_remove(g_ospf_work_local->instances, GUINT_TO_POINTER(process_id));
    apply->rc = OSPF_APPLY_RC_OK;
}

static void ospf_apply_router_id_set(ospf_apply_cmd_t *apply)
{
    ospf_instance_t *inst = ospf_lookup_instance(apply->u.router_id_set.process_id);
    if (!inst)
    {
        ospf_apply_fail(apply, "OSPF Error: process does not exist");
        return;
    }
    if (inst->router_id == apply->u.router_id_set.router_id)
    {
        apply->rc = OSPF_APPLY_RC_NOOP;
        return;
    }
    if (ospf_router_id_in_use(apply->u.router_id_set.router_id, inst->process_id))
    {
        ospf_apply_fail(apply, "OSPF Error: router ID is already used by another process");
        return;
    }

    ospf_packet_reset_instance(inst);
    ospf_worker_withdraw_all_routes(inst);
    g_hash_table_remove_all(inst->lsdb);
    inst->router_id = apply->u.router_id_set.router_id;
    inst->next_lsa_sequence = OSPF_LSA_INITIAL_SEQUENCE;
    inst->last_lsa_refresh_msec = 0u;
    inst->next_lsa_originate_msec = 0u;
    ospf_worker_reconcile_all();
    apply->rc = OSPF_APPLY_RC_OK;
}

static void ospf_apply_if_set(ospf_apply_cmd_t *apply)
{
    uint32_t process_id = apply->u.if_set.process_id;
    ospf_if_cfg_t *desired = &apply->u.if_set.cfg;
    ospf_instance_t *inst = ospf_lookup_instance(process_id);
    if (!inst)
    {
        ospf_apply_fail(apply, "OSPF Error: process does not exist");
        return;
    }
    if (desired->ifname[0] == '\0' || desired->network_type < OSPF_NETWORK_BROADCAST ||
        desired->network_type > OSPF_NETWORK_POINT_TO_POINT || desired->cost == 0u || desired->hello_interval == 0u ||
        desired->dead_interval <= desired->hello_interval)
    {
        ospf_apply_fail(apply, "OSPF Error: invalid interface configuration");
        return;
    }
    g_strlcpy(desired->vrf_name, inst->vrf_name, sizeof(desired->vrf_name));
    const if_api_cache_entry_t *if_entry = desired->enabled ? if_api_cache_lookup(desired->ifname) : NULL;
    if (if_entry && !ospf_if_entry_matches_vrf(inst->vrf_name, if_entry))
    {
        apply->rc = OSPF_APPLY_RC_FAIL;
        g_snprintf(apply->errmsg, sizeof(apply->errmsg), "OSPF Error: interface %s is not in VRF %s", desired->ifname,
                   inst->vrf_name);
        return;
    }
    uint32_t owner_process_id = desired->enabled ? ospf_interface_owner(desired->ifname, process_id) : 0u;
    if (owner_process_id != 0u)
    {
        ospf_apply_fail(apply, "OSPF Error: interface is already enabled in another process");
        return;
    }
    if (!ospf_instance_add_area(inst, desired->area_id))
    {
        ospf_apply_fail(apply, "OSPF Error: failed to create area");
        return;
    }

    ospf_if_cfg_t *current = g_hash_table_lookup(inst->if_cfgs, desired->ifname);
    if (current && ospf_if_config_equal(current, desired))
    {
        apply->rc = OSPF_APPLY_RC_NOOP;
        return;
    }
    if (current)
    {
        ospf_packet_remove_interface(inst, current->ifname);
    }

    ospf_if_cfg_t *next = g_memdup2(desired, sizeof(*next));
    if (!next)
    {
        ospf_apply_fail(apply, "OSPF Error: out of memory");
        return;
    }
    next->state = OSPF_IF_STATE_DOWN;
    next->dr = 0u;
    next->bdr = 0u;
    next->joined_all_routers = 0u;
    next->joined_dr_routers = 0u;
    next->last_hello_tx_msec = 0u;
    next->wait_until_msec = 0u;
    g_hash_table_replace(inst->if_cfgs, g_strdup(next->ifname), next);

    ospf_worker_reconcile_all();
    apply->rc = OSPF_APPLY_RC_OK;
}

static void ospf_apply_if_del(ospf_apply_cmd_t *apply)
{
    uint32_t process_id = apply->u.if_del.process_id;
    ospf_instance_t *inst = ospf_lookup_instance(process_id);
    if (!inst)
    {
        apply->rc = OSPF_APPLY_RC_NOOP;
        return;
    }

    ospf_if_cfg_t *current = g_hash_table_lookup(inst->if_cfgs, apply->u.if_del.ifname);
    if (!current)
    {
        apply->rc = OSPF_APPLY_RC_NOOP;
        return;
    }
    ospf_packet_remove_interface(inst, current->ifname);
    g_hash_table_remove(inst->if_cfgs, apply->u.if_del.ifname);
    ospf_lsa_originate_all(inst, ospf_now_msec());
    ospf_spf_recalculate(inst);
    apply->rc = OSPF_APPLY_RC_OK;
}

static void ospf_apply_area_set(ospf_apply_cmd_t *apply)
{
    ospf_instance_t *inst = ospf_lookup_instance(apply->u.area_set.process_id);
    if (!inst)
    {
        ospf_apply_fail(apply, "OSPF Error: process does not exist");
        return;
    }
    if (ospf_instance_has_area(inst, apply->u.area_set.area_id))
    {
        apply->rc = OSPF_APPLY_RC_NOOP;
        return;
    }
    if (!ospf_instance_add_area(inst, apply->u.area_set.area_id))
    {
        ospf_apply_fail(apply, "OSPF Error: failed to create area");
        return;
    }
    apply->rc = OSPF_APPLY_RC_OK;
}

static void ospf_apply_area_del(ospf_apply_cmd_t *apply)
{
    ospf_instance_t *inst = ospf_lookup_instance(apply->u.area_del.process_id);
    if (!inst || !ospf_instance_has_area(inst, apply->u.area_del.area_id))
    {
        apply->rc = OSPF_APPLY_RC_NOOP;
        return;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_if_cfg_t *cfg = (const ospf_if_cfg_t *)value;
        if (cfg->area_id == apply->u.area_del.area_id)
        {
            ospf_apply_fail(apply, "OSPF Error: area is referenced by an interface");
            return;
        }
    }

    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_neighbor_t *nbr = (const ospf_neighbor_t *)value;
        if (nbr->area_id == apply->u.area_del.area_id)
        {
            g_hash_table_iter_remove(&iter);
        }
    }

    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_lsa_entry_t *entry = (const ospf_lsa_entry_t *)value;
        if (entry->area_id == apply->u.area_del.area_id)
        {
            g_hash_table_iter_remove(&iter);
        }
    }

    g_hash_table_remove(inst->areas, &apply->u.area_del.area_id);
    ospf_spf_recalculate(inst);
    apply->rc = OSPF_APPLY_RC_OK;
}

static void ospf_worker_apply(ospf_apply_cmd_t *apply)
{
    apply->rc = OSPF_APPLY_RC_FAIL;
    apply->errmsg[0] = '\0';

    switch (apply->op)
    {
        case OSPF_APPLY_INSTANCE_SET:
            ospf_apply_instance_set(apply);
            break;
        case OSPF_APPLY_INSTANCE_DEL:
            ospf_apply_instance_del(apply);
            break;
        case OSPF_APPLY_ROUTER_ID_SET:
            ospf_apply_router_id_set(apply);
            break;
        case OSPF_APPLY_IF_SET:
            ospf_apply_if_set(apply);
            break;
        case OSPF_APPLY_IF_DEL:
            ospf_apply_if_del(apply);
            break;
        case OSPF_APPLY_AREA_SET:
            ospf_apply_area_set(apply);
            break;
        case OSPF_APPLY_AREA_DEL:
            ospf_apply_area_del(apply);
            break;
        default:
            ospf_apply_fail(apply, "OSPF Error: unknown apply operation");
            break;
    }
}

static uint32_t ospf_select_router_id(const ospf_instance_t *inst)
{
    uint32_t best = 0u;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_if_cfg_t *cfg = (const ospf_if_cfg_t *)value;
        const if_api_cache_entry_t *entry = cfg->enabled ? if_api_cache_lookup(cfg->ifname) : NULL;
        if (!ospf_if_entry_matches_vrf(inst->vrf_name, entry) || entry->ipv4_addr.family != AF_INET)
        {
            continue;
        }
        uint32_t candidate = ntohl(entry->ipv4_addr.u.v4.s_addr);
        if (candidate > best && !ospf_router_id_in_use(candidate, inst->process_id))
        {
            best = candidate;
        }
    }
    return best;
}

static void ospf_reconcile_instance(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    (void)user_data;
    ospf_instance_t *inst = (ospf_instance_t *)value;
    if (!inst || !inst->admin_up)
    {
        return;
    }

    if (inst->router_id == 0u)
    {
        uint32_t selected = ospf_select_router_id(inst);
        if (selected != 0u)
        {
            inst->router_id = selected;
            LOG_INFO("OSPF process %u selected router ID %u.%u.%u.%u", inst->process_id,
                     (inst->router_id >> 24) & 0xffu, (inst->router_id >> 16) & 0xffu, (inst->router_id >> 8) & 0xffu,
                     inst->router_id & 0xffu);
        }
    }

    GHashTableIter iter;
    gpointer if_value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &if_value))
    {
        ospf_if_cfg_t *cfg = (ospf_if_cfg_t *)if_value;
        const if_api_cache_entry_t *if_entry = if_api_cache_lookup(cfg->ifname);
        gboolean ready = cfg->enabled && inst->router_id != 0u && ospf_if_entry_matches_vrf(inst->vrf_name, if_entry) &&
                         if_entry->link_up && if_entry->proto_up && if_entry->ifindex != 0u &&
                         if_entry->ipv4_addr.family == AF_INET;
        if (ready)
        {
            ospf_packet_reconcile_interface(inst, cfg);
        }
        else
        {
            ospf_packet_remove_interface(inst, cfg->ifname);
        }
    }

    ospf_lsa_originate_all(inst, ospf_now_msec());
    ospf_spf_recalculate(inst);
}

void ospf_worker_reconcile_all(void)
{
    if (g_ospf_work_local && g_ospf_work_local->instances)
    {
        g_hash_table_foreach(g_ospf_work_local->instances, ospf_reconcile_instance, NULL);
    }
}

static void ospf_replay_instance(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    ospf_instance_t *inst = (ospf_instance_t *)value;
    uint32_t *count = (uint32_t *)user_data;
    GHashTableIter iter;
    gpointer route_value = NULL;
    g_hash_table_iter_init(&iter, inst->routes);
    while (g_hash_table_iter_next(&iter, NULL, &route_value))
    {
        if (ospf_route_sync_enqueue_add((const ospf_route_t *)route_value) == ERRCODE_SUCCESS)
        {
            (*count)++;
        }
    }
}

void ospf_worker_replay_routes(void)
{
    uint32_t count = 0u;
    if (g_ospf_work_local && g_ospf_work_local->instances)
    {
        g_hash_table_foreach(g_ospf_work_local->instances, ospf_replay_instance, &count);
    }
    LOG_INFO("OSPF: queued %u desired route(s) for ROUTE replay", count);
}

static void ospf_worker_handle_if_down(void)
{
    if (!g_ospf_work_local || !g_ospf_work_local->instances)
    {
        return;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_ospf_work_local->instances);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        ospf_instance_t *inst = (ospf_instance_t *)value;
        ospf_packet_reset_instance(inst);
        ospf_worker_withdraw_all_routes(inst);
        g_hash_table_remove_all(inst->lsdb);
    }
    if_api_cache_cleanup();
    if_api_cache_init();
}

static int ospf_worker_dispatch(ospf_worker_cmd_t *cmd)
{
    switch (cmd->type)
    {
        case OSPF_WORKER_CMD_SHOW:
            (void)ospf_show_handle_msg(cmd->msg);
            cmd->msg = NULL;
            break;
        case OSPF_WORKER_CMD_IF_EVENT:
            if_api_cache_on_event(cmd->msg);
            ospf_worker_reconcile_all();
            dev_ipc_message_free(cmd->msg);
            cmd->msg = NULL;
            break;
        case OSPF_WORKER_CMD_APPLY:
            ospf_worker_apply(cmd->apply);
            ospf_worker_cmd_complete(cmd, ERRCODE_SUCCESS);
            return 0;
        case OSPF_WORKER_CMD_ROUTE_READY:
            ospf_worker_replay_routes();
            break;
        case OSPF_WORKER_CMD_IF_DOWN:
            ospf_worker_handle_if_down();
            break;
        case OSPF_WORKER_CMD_SHUTDOWN:
            g_ospf_work_local->running = 0;
            g_free(cmd);
            return 1;
        default:
            break;
    }
    g_free(cmd);
    return 0;
}

static int ospf_worker_drain_commands(void)
{
    uint64_t value;
    while (read(g_ospf_work_local->cmd_eventfd, &value, sizeof(value)) > 0)
    {
    }

    ospf_worker_cmd_t *cmd = NULL;
    while ((cmd = g_async_queue_try_pop(g_ospf_work_local->cmd_queue)) != NULL)
    {
        if (ospf_worker_dispatch(cmd))
        {
            return 1;
        }
    }
    return 0;
}

static void ospf_worker_handle_timer(void)
{
    uint64_t expirations;
    if (read(g_ospf_work_local->timer_fd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("OSPF: timerfd read failed");
    }

    uint64_t now_msec = ospf_now_msec();
    ospf_packet_tick(now_msec);
    if (g_ospf_work_local->instances)
    {
        GHashTableIter iter;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, g_ospf_work_local->instances);
        while (g_hash_table_iter_next(&iter, NULL, &value))
        {
            ospf_lsa_age((ospf_instance_t *)value, now_msec);
        }
    }
}

static void *ospf_worker_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "ospf-worker");
    log_set_tag("ospf");

    struct epoll_event events[OSPF_MAX_EPOLL_EVENTS];
    while (g_ospf_work_local->running)
    {
        int count = epoll_wait(g_ospf_work_local->epoll_fd, events, OSPF_MAX_EPOLL_EVENTS, 1000);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("OSPF: epoll_wait failed");
            break;
        }
        for (int i = 0; i < count; ++i)
        {
            if (events[i].data.ptr == &g_ospf_cmd_tag)
            {
                if (ospf_worker_drain_commands())
                {
                    return NULL;
                }
            }
            else if (events[i].data.ptr == &g_ospf_raw_tag)
            {
                ospf_packet_handle_read();
            }
            else if (events[i].data.ptr == &g_ospf_timer_tag)
            {
                ospf_worker_handle_timer();
            }
        }
    }
    return NULL;
}

static int ospf_epoll_add(int fd, void *tag)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = tag;
    return epoll_ctl(g_ospf_work_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

int ospf_worker_prepare(void)
{
    g_ospf_work_local = g_malloc0(sizeof(*g_ospf_work_local));
    if (!g_ospf_work_local)
    {
        return ERRCODE_FAIL;
    }
    g_ospf_work_local->epoll_fd = -1;
    g_ospf_work_local->cmd_eventfd = -1;
    g_ospf_work_local->timer_fd = -1;
    g_ospf_work_local->raw_fd = -1;

    g_ospf_work_local->instances = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, ospf_instance_free);
    g_ospf_work_local->cmd_queue = g_async_queue_new();
    if_api_cache_init();

    g_ospf_work_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    g_ospf_work_local->cmd_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    g_ospf_work_local->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    g_ospf_work_local->raw_fd = ospf_packet_socket_open();
    if (!g_ospf_work_local->instances || !g_ospf_work_local->cmd_queue || g_ospf_work_local->epoll_fd < 0 ||
        g_ospf_work_local->cmd_eventfd < 0 || g_ospf_work_local->timer_fd < 0 || g_ospf_work_local->raw_fd < 0)
    {
        LOG_ERROR("OSPF: worker resource allocation failed");
        return ERRCODE_FAIL;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_sec = 1;
    its.it_value.tv_sec = 1;
    if (timerfd_settime(g_ospf_work_local->timer_fd, 0, &its, NULL) < 0 ||
        ospf_epoll_add(g_ospf_work_local->cmd_eventfd, &g_ospf_cmd_tag) < 0 ||
        ospf_epoll_add(g_ospf_work_local->timer_fd, &g_ospf_timer_tag) < 0 ||
        ospf_epoll_add(g_ospf_work_local->raw_fd, &g_ospf_raw_tag) < 0 || ospf_route_sync_prepare() != ERRCODE_SUCCESS)
    {
        LOG_PERROR("OSPF: failed to register worker descriptors");
        return ERRCODE_FAIL;
    }

    g_ospf_work_local->running = 1;
    return ERRCODE_SUCCESS;
}

int ospf_worker_launch(void)
{
    if (!g_ospf_work_local || ospf_route_sync_launch() != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (pthread_create(&g_ospf_work_local->thread, NULL, ospf_worker_thread, NULL) != 0)
    {
        ospf_route_sync_shutdown();
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

static int ospf_worker_post(ospf_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    ospf_worker_cmd_t *cmd = ospf_worker_cmd_create(type, msg, 0);
    if (!cmd || ospf_worker_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        ospf_worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int ospf_worker_post_show_cli(dev_ipc_message_t *msg)
{
    return ospf_worker_post(OSPF_WORKER_CMD_SHOW, msg);
}

int ospf_worker_post_if_event(dev_ipc_message_t *msg)
{
    return ospf_worker_post(OSPF_WORKER_CMD_IF_EVENT, msg);
}

int ospf_worker_post_route_ready(void)
{
    return ospf_worker_post(OSPF_WORKER_CMD_ROUTE_READY, NULL);
}

int ospf_worker_post_if_down(void)
{
    return ospf_worker_post(OSPF_WORKER_CMD_IF_DOWN, NULL);
}

int ospf_worker_dispatch_apply(ospf_apply_cmd_t *apply)
{
    if (!apply)
    {
        return ERRCODE_FAIL;
    }
    ospf_worker_cmd_t *cmd = ospf_worker_cmd_create(OSPF_WORKER_CMD_APPLY, NULL, 1);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    cmd->apply = apply;
    if (ospf_worker_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        ospf_worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    int rc = ospf_worker_cmd_wait(cmd);
    ospf_worker_cmd_destroy(cmd);
    return rc;
}

void ospf_worker_shutdown(void)
{
    if (!g_ospf_work_local)
    {
        return;
    }

    if (g_ospf_work_local->running && g_ospf_work_local->thread)
    {
        ospf_worker_cmd_t *cmd = ospf_worker_cmd_create(OSPF_WORKER_CMD_SHUTDOWN, NULL, 0);
        if (cmd)
        {
            if (ospf_worker_enqueue(cmd) != ERRCODE_SUCCESS)
            {
                ospf_worker_cmd_destroy(cmd);
                g_ospf_work_local->running = 0;
            }
        }
        else
        {
            g_ospf_work_local->running = 0;
        }
        pthread_join(g_ospf_work_local->thread, NULL);
        g_ospf_work_local->thread = 0;
    }

    if (g_ospf_work_local->instances)
    {
        g_hash_table_destroy(g_ospf_work_local->instances);
        g_ospf_work_local->instances = NULL;
    }
    ospf_route_sync_shutdown();
    ospf_show_cleanup();
    ospf_packet_socket_close();

    if (g_ospf_work_local->timer_fd >= 0)
    {
        close(g_ospf_work_local->timer_fd);
    }
    if (g_ospf_work_local->cmd_eventfd >= 0)
    {
        close(g_ospf_work_local->cmd_eventfd);
    }
    if (g_ospf_work_local->epoll_fd >= 0)
    {
        close(g_ospf_work_local->epoll_fd);
    }
    if (g_ospf_work_local->cmd_queue)
    {
        ospf_worker_cmd_t *cmd;
        while ((cmd = g_async_queue_try_pop(g_ospf_work_local->cmd_queue)) != NULL)
        {
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
            }
            if (cmd->waitable && !cmd->done)
            {
                ospf_worker_cmd_complete(cmd, ERRCODE_FAIL);
            }
            if (!cmd->waitable)
            {
                ospf_worker_cmd_destroy(cmd);
            }
        }
        g_async_queue_unref(g_ospf_work_local->cmd_queue);
    }

    if_api_cache_cleanup();
    g_free(g_ospf_work_local);
    g_ospf_work_local = NULL;
}
