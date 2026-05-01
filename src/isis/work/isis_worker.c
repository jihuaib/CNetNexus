/**
 * @file   isis_worker.c
 * @brief  ISIS worker 线程实现
 * @author jhb
 * @date   2026/04/11
 */
#include "isis_worker.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "errcode.h"
#include "if.h"
#include "isis.h"
#include "isis_neighbor.h"
#include "isis_route.h"
#include "isis_route_sync.h"
#include "isis_show.h"
#include "log.h"

#define ISIS_MAX_EPOLL_EVENTS 8

typedef enum isis_worker_cmd_type
{
    ISIS_WORKER_CMD_SHOW = 1,
    ISIS_WORKER_CMD_IF_EVENT = 2,
    ISIS_WORKER_CMD_APPLY = 3,
    ISIS_WORKER_CMD_SHUTDOWN = 4,
} isis_worker_cmd_type_t;

typedef struct isis_worker_cmd
{
    isis_worker_cmd_type_t type;
    dev_ipc_message_t *msg;
    isis_apply_cmd_t *apply;

    int waitable;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int rc;
} isis_worker_cmd_t;

static char g_isis_cmd_tag;
static char g_isis_raw_tag;
static char g_isis_tick_tag;

isis_work_local_t *g_isis_work_local = NULL;

static void isis_if_cfg_free(gpointer data)
{
    g_free(data);
}

static void isis_route_state_free(gpointer data)
{
    g_free(data);
}

static void isis_neighbor_free(gpointer data)
{
    g_free(data);
}

static void isis_lsdb_entry_free(gpointer data)
{
    isis_lsdb_entry_t *entry = (isis_lsdb_entry_t *)data;
    if (!entry)
    {
        return;
    }
    if (entry->tlvs)
    {
        g_byte_array_free(entry->tlvs, TRUE);
        entry->tlvs = NULL;
    }
    g_free(entry);
}

static void isis_instance_cfg_free(gpointer data)
{
    isis_instance_cfg_t *inst = (isis_instance_cfg_t *)data;
    if (!inst)
    {
        return;
    }
    if (inst->if_cfgs)
    {
        g_hash_table_destroy(inst->if_cfgs);
        inst->if_cfgs = NULL;
    }
    if (inst->route_states)
    {
        g_hash_table_destroy(inst->route_states);
        inst->route_states = NULL;
    }
    if (inst->learned_route_heads)
    {
        g_hash_table_destroy(inst->learned_route_heads);
        inst->learned_route_heads = NULL;
    }
    if (inst->neighbors)
    {
        g_hash_table_destroy(inst->neighbors);
        inst->neighbors = NULL;
    }
    if (inst->lsdb_entries)
    {
        g_hash_table_destroy(inst->lsdb_entries);
        inst->lsdb_entries = NULL;
    }
    g_free(inst);
}

static isis_instance_cfg_t *isis_instance_create(uint32_t tag)
{
    isis_instance_cfg_t *inst = g_malloc0(sizeof(*inst));
    if (!inst)
    {
        return NULL;
    }

    inst->tag = tag;
    inst->is_type = ISIS_IS_TYPE_LEVEL_1_2;
    inst->admin_up = 1u;
    inst->af_ipv4 = 1u;
    inst->af_ipv6 = 1u;
    inst->if_cfgs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_if_cfg_free);
    if (!inst->if_cfgs)
    {
        g_free(inst);
        return NULL;
    }
    inst->route_states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_route_state_free);
    if (!inst->route_states)
    {
        g_hash_table_destroy(inst->if_cfgs);
        inst->if_cfgs = NULL;
        g_free(inst);
        return NULL;
    }
    inst->learned_route_heads = isis_route_head_table_new();
    if (!inst->learned_route_heads)
    {
        g_hash_table_destroy(inst->route_states);
        inst->route_states = NULL;
        g_hash_table_destroy(inst->if_cfgs);
        inst->if_cfgs = NULL;
        g_free(inst);
        return NULL;
    }
    inst->neighbors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_neighbor_free);
    if (!inst->neighbors)
    {
        g_hash_table_destroy(inst->learned_route_heads);
        inst->learned_route_heads = NULL;
        g_hash_table_destroy(inst->route_states);
        inst->route_states = NULL;
        g_hash_table_destroy(inst->if_cfgs);
        inst->if_cfgs = NULL;
        g_free(inst);
        return NULL;
    }

    inst->lsdb_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_lsdb_entry_free);
    if (!inst->lsdb_entries)
    {
        g_hash_table_destroy(inst->neighbors);
        inst->neighbors = NULL;
        g_hash_table_destroy(inst->learned_route_heads);
        inst->learned_route_heads = NULL;
        g_hash_table_destroy(inst->route_states);
        inst->route_states = NULL;
        g_hash_table_destroy(inst->if_cfgs);
        inst->if_cfgs = NULL;
        g_free(inst);
        return NULL;
    }
    return inst;
}

static isis_instance_cfg_t *isis_lookup_instance(uint32_t tag)
{
    if (!g_isis_work_local || !g_isis_work_local->instances || tag == 0u)
    {
        return NULL;
    }
    return (isis_instance_cfg_t *)g_hash_table_lookup(g_isis_work_local->instances, GUINT_TO_POINTER(tag));
}

static isis_instance_cfg_t *isis_get_or_create_instance(uint32_t tag)
{
    isis_instance_cfg_t *inst = isis_lookup_instance(tag);
    if (inst)
    {
        return inst;
    }

    inst = isis_instance_create(tag);
    if (!inst)
    {
        return NULL;
    }

    g_hash_table_insert(g_isis_work_local->instances, GUINT_TO_POINTER(tag), inst);
    return inst;
}

static isis_worker_cmd_t *worker_cmd_create(isis_worker_cmd_type_t type, dev_ipc_message_t *msg, int waitable)
{
    isis_worker_cmd_t *cmd = g_malloc0(sizeof(*cmd));
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

static void worker_cmd_destroy(isis_worker_cmd_t *cmd)
{
    if (!cmd)
    {
        return;
    }
    if (cmd->waitable)
    {
        pthread_mutex_destroy(&cmd->mutex);
        pthread_cond_destroy(&cmd->cond);
    }
    g_free(cmd);
}

static void worker_cmd_complete(isis_worker_cmd_t *cmd, int rc)
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

static int worker_cmd_wait(isis_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->waitable)
    {
        return ERRCODE_FAIL;
    }

    pthread_mutex_lock(&cmd->mutex);
    while (!cmd->done)
    {
        pthread_cond_wait(&cmd->cond, &cmd->mutex);
    }
    int rc = cmd->rc;
    pthread_mutex_unlock(&cmd->mutex);
    return rc;
}

static void worker_signal_cmd_event(void)
{
    if (!g_isis_work_local || g_isis_work_local->cmd_eventfd < 0)
    {
        return;
    }

    uint64_t one = 1;
    if (write(g_isis_work_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("ISIS: write cmd_eventfd failed");
        }
    }
}

static int worker_cmd_enqueue(isis_worker_cmd_t *cmd)
{
    if (!cmd || !g_isis_work_local || !g_isis_work_local->cmd_queue || g_isis_work_local->cmd_eventfd < 0)
    {
        return -1;
    }

    g_async_queue_push(g_isis_work_local->cmd_queue, cmd);
    worker_signal_cmd_event();
    return 0;
}

static int worker_apply_cmd(isis_apply_cmd_t *apply)
{
    if (!apply)
    {
        return ERRCODE_FAIL;
    }

    switch (apply->op)
    {
        case ISIS_APPLY_OP_INSTANCE_SET:
        {
            isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.instance_set.tag);
            if (inst)
            {
                g_strlcpy(inst->net, apply->u.instance_set.net, sizeof(inst->net));
                inst->is_type = apply->u.instance_set.is_type;
                inst->admin_up = apply->u.instance_set.admin_up ? 1u : 0u;
                isis_neighbor_reconcile_instance(inst);
                isis_route_sync_reconcile_instance_all_if(inst);
                return ERRCODE_SUCCESS;
            }

            inst = isis_get_or_create_instance(apply->u.instance_set.tag);
            if (!inst)
            {
                return ERRCODE_FAIL;
            }
            g_strlcpy(inst->net, apply->u.instance_set.net, sizeof(inst->net));
            inst->is_type = apply->u.instance_set.is_type;
            inst->admin_up = apply->u.instance_set.admin_up ? 1u : 0u;
            isis_neighbor_reconcile_instance(inst);
            isis_route_sync_reconcile_instance_all_if(inst);
            return ERRCODE_SUCCESS;
        }

        case ISIS_APPLY_OP_INSTANCE_DEL:
        {
            isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.instance_del.tag);
            if (!inst)
            {
                return ERRCODE_FAIL;
            }
            isis_route_sync_withdraw_all_instance_routes(inst);
            return g_hash_table_remove(g_isis_work_local->instances, GUINT_TO_POINTER(apply->u.instance_del.tag))
                       ? ERRCODE_SUCCESS
                       : ERRCODE_FAIL;
        }

        case ISIS_APPLY_OP_NET_SET:
        {
            isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.net_set.tag);
            if (!inst)
            {
                return ERRCODE_FAIL;
            }
            g_strlcpy(inst->net, apply->u.net_set.net, sizeof(inst->net));
            return ERRCODE_SUCCESS;
        }

        case ISIS_APPLY_OP_IS_TYPE_SET:
        {
            isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.is_type_set.tag);
            if (!inst)
            {
                return ERRCODE_FAIL;
            }
            inst->is_type = apply->u.is_type_set.is_type;
            isis_neighbor_reconcile_instance(inst);
            return ERRCODE_SUCCESS;
        }

        case ISIS_APPLY_OP_AF_SET:
        {
            isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.af_set.tag);
            if (!inst)
            {
                return ERRCODE_FAIL;
            }
            if (apply->u.af_set.afi == ISIS_AFI_IPV4)
            {
                inst->af_ipv4 = 1u;
                isis_neighbor_reconcile_instance(inst);
                isis_route_sync_reconcile_instance_all_if(inst);
                return ERRCODE_SUCCESS;
            }
            if (apply->u.af_set.afi == ISIS_AFI_IPV6)
            {
                inst->af_ipv6 = 1u;
                isis_neighbor_reconcile_instance(inst);
                isis_route_sync_reconcile_instance_all_if(inst);
                return ERRCODE_SUCCESS;
            }
            return ERRCODE_FAIL;
        }

        case ISIS_APPLY_OP_AF_DEL:
        {
            isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.af_del.tag);
            if (!inst)
            {
                return ERRCODE_FAIL;
            }
            if (apply->u.af_del.afi == ISIS_AFI_IPV4)
            {
                inst->af_ipv4 = 0u;
                isis_neighbor_reconcile_instance(inst);
                isis_route_sync_reconcile_instance_all_if(inst);
                return ERRCODE_SUCCESS;
            }
            if (apply->u.af_del.afi == ISIS_AFI_IPV6)
            {
                inst->af_ipv6 = 0u;
                isis_neighbor_reconcile_instance(inst);
                isis_route_sync_reconcile_instance_all_if(inst);
                return ERRCODE_SUCCESS;
            }
            return ERRCODE_FAIL;
        }

        case ISIS_APPLY_OP_IF_SET:
        {
            isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.if_set.tag);
            if (!inst || apply->u.if_set.cfg.ifname[0] == '\0')
            {
                return ERRCODE_FAIL;
            }

            if (!isis_if_cfg_any_enabled(&apply->u.if_set.cfg))
            {
                (void)g_hash_table_remove(inst->if_cfgs, apply->u.if_set.cfg.ifname);
                isis_neighbor_on_if_removed(inst, apply->u.if_set.cfg.ifname);
                isis_route_sync_reconcile_instance_if(inst, apply->u.if_set.cfg.ifname);
                return ERRCODE_SUCCESS;
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
                    return ERRCODE_FAIL;
                }
                *cfg = apply->u.if_set.cfg;
                g_hash_table_insert(inst->if_cfgs, g_strdup(cfg->ifname), cfg);
            }

            isis_neighbor_reconcile_if(cfg->ifname);
            isis_route_sync_reconcile_instance_if(inst, cfg->ifname);
            return ERRCODE_SUCCESS;
        }

        case ISIS_APPLY_OP_IF_DEL:
        {
            isis_instance_cfg_t *inst = isis_lookup_instance(apply->u.if_del.tag);
            if (!inst || apply->u.if_del.ifname[0] == '\0')
            {
                return ERRCODE_FAIL;
            }
            int removed = g_hash_table_remove(inst->if_cfgs, apply->u.if_del.ifname) ? 1 : 0;
            isis_neighbor_on_if_removed(inst, apply->u.if_del.ifname);
            isis_route_sync_reconcile_instance_if(inst, apply->u.if_del.ifname);
            return removed ? ERRCODE_SUCCESS : ERRCODE_FAIL;
        }

        default:
            return ERRCODE_FAIL;
    }
}

static int worker_dispatch_cmd(isis_worker_cmd_t *cmd)
{
    if (!cmd)
    {
        return 0;
    }

    switch (cmd->type)
    {
        case ISIS_WORKER_CMD_SHOW:
            (void)isis_show_handle_msg(cmd->msg);
            cmd->msg = NULL;
            break;

        case ISIS_WORKER_CMD_IF_EVENT:
        {
            char ifname[IF_LOGICAL_NAME_MAX] = {0};
            const char *logical_name = isis_route_sync_if_event_get_logical_name(cmd->msg, ifname, sizeof(ifname));
            if_api_cache_on_event(cmd->msg);
            if (logical_name && logical_name[0] != '\0')
            {
                isis_neighbor_reconcile_if(logical_name);
                isis_route_sync_reconcile_all_instances_if(logical_name);
            }
            else
            {
                isis_neighbor_reconcile_all();
                isis_route_sync_reconcile_all_instances();
            }
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            break;
        }

        case ISIS_WORKER_CMD_APPLY:
            if (cmd->apply)
            {
                cmd->apply->rc = worker_apply_cmd(cmd->apply);
            }
            worker_cmd_complete(cmd, ERRCODE_SUCCESS);
            return 0;

        case ISIS_WORKER_CMD_SHUTDOWN:
            g_isis_work_local->running = 0;
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            g_free(cmd);
            return 1;

        default:
            break;
    }

    g_free(cmd);
    return 0;
}

static int worker_drain_cmd_queue(void)
{
    uint64_t v;
    while (read(g_isis_work_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
        /* drain */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("ISIS: cmd eventfd read failed");
    }

    isis_worker_cmd_t *cmd = NULL;
    while ((cmd = (isis_worker_cmd_t *)g_async_queue_try_pop(g_isis_work_local->cmd_queue)) != NULL)
    {
        if (worker_dispatch_cmd(cmd))
        {
            return 1;
        }
    }
    return 0;
}

static void *isis_worker_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "isis-worker");
    log_set_tag("isis");

    struct epoll_event events[ISIS_MAX_EPOLL_EVENTS];

    while (g_isis_work_local->running)
    {
        int n = epoll_wait(g_isis_work_local->epoll_fd, events, ISIS_MAX_EPOLL_EVENTS, 1000);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("ISIS: epoll_wait failed");
            break;
        }

        for (int i = 0; i < n; i++)
        {
            if (events[i].data.ptr == (void *)&g_isis_cmd_tag)
            {
                if (worker_drain_cmd_queue())
                {
                    return NULL;
                }
            }
            else if (events[i].data.ptr == (void *)&g_isis_raw_tag)
            {
                isis_neighbor_handle_raw_event();
            }
            else if (events[i].data.ptr == (void *)&g_isis_tick_tag)
            {
                isis_neighbor_handle_tick_event();
            }
        }
    }

    return NULL;
}

int isis_worker_prepare(void)
{
    if (!g_isis_work_local)
    {
        g_isis_work_local = g_malloc0(sizeof(*g_isis_work_local));
        if (!g_isis_work_local)
        {
            return ERRCODE_FAIL;
        }
    }

    g_isis_work_local->instances =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)isis_instance_cfg_free);
    if (!g_isis_work_local->instances)
    {
        return ERRCODE_FAIL;
    }

    if_api_cache_init();

    g_isis_work_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_isis_work_local->epoll_fd < 0)
    {
        return ERRCODE_FAIL;
    }

    g_isis_work_local->cmd_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_isis_work_local->cmd_eventfd < 0)
    {
        return ERRCODE_FAIL;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_isis_cmd_tag;
    if (epoll_ctl(g_isis_work_local->epoll_fd, EPOLL_CTL_ADD, g_isis_work_local->cmd_eventfd, &ev) < 0)
    {
        return ERRCODE_FAIL;
    }

    g_isis_work_local->cmd_queue = g_async_queue_new();
    if (!g_isis_work_local->cmd_queue)
    {
        return ERRCODE_FAIL;
    }

    (void)isis_neighbor_prepare(g_isis_work_local->epoll_fd, &g_isis_raw_tag, &g_isis_tick_tag);

    g_isis_work_local->running = 1;
    return ERRCODE_SUCCESS;
}

int isis_worker_launch(void)
{
    if (!g_isis_work_local)
    {
        return ERRCODE_FAIL;
    }

    if (pthread_create(&g_isis_work_local->thread, NULL, isis_worker_thread_fn, NULL) != 0)
    {
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int isis_worker_post_show_cli(dev_ipc_message_t *msg)
{
    isis_worker_cmd_t *cmd = worker_cmd_create(ISIS_WORKER_CMD_SHOW, msg, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int isis_worker_post_if_event(dev_ipc_message_t *msg)
{
    isis_worker_cmd_t *cmd = worker_cmd_create(ISIS_WORKER_CMD_IF_EVENT, msg, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int isis_worker_dispatch_apply(isis_apply_cmd_t *apply)
{
    if (!apply)
    {
        return ERRCODE_FAIL;
    }

    isis_worker_cmd_t *cmd = worker_cmd_create(ISIS_WORKER_CMD_APPLY, NULL, 1);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    cmd->apply = apply;

    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }

    (void)worker_cmd_wait(cmd);
    worker_cmd_destroy(cmd);
    return ERRCODE_SUCCESS;
}

void isis_worker_shutdown(void)
{
    if (!g_isis_work_local)
    {
        return;
    }

    if (g_isis_work_local->running && g_isis_work_local->thread != 0)
    {
        isis_worker_cmd_t *cmd = g_malloc0(sizeof(*cmd));
        if (cmd)
        {
            cmd->type = ISIS_WORKER_CMD_SHUTDOWN;
            g_async_queue_push(g_isis_work_local->cmd_queue, cmd);
            worker_signal_cmd_event();
        }
        pthread_join(g_isis_work_local->thread, NULL);
        g_isis_work_local->thread = 0;
    }

    if (g_isis_work_local->cmd_queue)
    {
        isis_worker_cmd_t *cmd = NULL;
        while ((cmd = (isis_worker_cmd_t *)g_async_queue_try_pop(g_isis_work_local->cmd_queue)) != NULL)
        {
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
            }
            if (cmd->waitable && !cmd->done)
            {
                worker_cmd_complete(cmd, ERRCODE_FAIL);
            }
            if (!cmd->waitable)
            {
                worker_cmd_destroy(cmd);
            }
        }
        g_async_queue_unref(g_isis_work_local->cmd_queue);
        g_isis_work_local->cmd_queue = NULL;
    }

    if (g_isis_work_local->cmd_eventfd >= 0)
    {
        close(g_isis_work_local->cmd_eventfd);
        g_isis_work_local->cmd_eventfd = -1;
    }

    isis_neighbor_shutdown(g_isis_work_local->epoll_fd);

    if (g_isis_work_local->epoll_fd >= 0)
    {
        close(g_isis_work_local->epoll_fd);
        g_isis_work_local->epoll_fd = -1;
    }

    isis_show_cleanup();

    if (g_isis_work_local->instances)
    {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, g_isis_work_local->instances);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            (void)key;
            isis_route_sync_withdraw_all_instance_routes((isis_instance_cfg_t *)value);
        }
        g_hash_table_destroy(g_isis_work_local->instances);
        g_isis_work_local->instances = NULL;
    }

    if_api_cache_cleanup();

    g_free(g_isis_work_local);
    g_isis_work_local = NULL;
}
