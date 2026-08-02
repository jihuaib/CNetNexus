/**
 * @file   isis_worker.c
 * @brief  ISIS worker 线程实现
 * @author jhb
 * @date   2026/04/11
 */
#include "isis_worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "errcode.h"
#include "if.h"
#include "isis.h"
#include "isis_cfg_apply.h"
#include "isis_main.h"
#include "isis_neighbor.h"
#include "isis_nexthop.h"
#include "isis_route.h"
#include "isis_route_sync.h"
#include "isis_show.h"
#include "log.h"
#include "route.h"

#define ISIS_MAX_EPOLL_EVENTS 8

typedef enum isis_worker_cmd_type
{
    ISIS_WORKER_CMD_SHOW = 1,
    ISIS_WORKER_CMD_IF_EVENT = 2,
    ISIS_WORKER_CMD_APPLY = 3,
    ISIS_WORKER_CMD_SHUTDOWN = 4,
    ISIS_WORKER_CMD_ROUTE_READY = 5,
    ISIS_WORKER_CMD_IF_DOWN = 6,
    ISIS_WORKER_CMD_ROUTE_DOWN = 7,
    ISIS_WORKER_CMD_ROUTE_MSG = 8,
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

static void isis_srv6_locator_mark_lsp_dirty(const char *locator_name)
{
    if (!g_isis_work_local || !g_isis_work_local->instances)
    {
        return;
    }
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_isis_work_local->instances);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        isis_instance_cfg_t *inst = value;
        if (inst && inst->vrf_id == ROUTE_VRF_DEFAULT && inst->af_ipv6 && inst->srv6_locator[0] != '\0' &&
            (!locator_name || strcmp(inst->srv6_locator, locator_name) == 0))
        {
            inst->last_lsp_tx_msec = 0u;
        }
    }
}

static void isis_srv6_locator_clear(void)
{
    if (!g_isis_work_local || !g_isis_work_local->srv6_locators)
    {
        return;
    }
    if (g_hash_table_size(g_isis_work_local->srv6_locators) > 0u)
    {
        g_hash_table_remove_all(g_isis_work_local->srv6_locators);
        isis_srv6_locator_mark_lsp_dirty(NULL);
    }
}

static int isis_srv6_locator_subscribe(void)
{
    route_subscribe_req_t *req = g_new0(route_subscribe_req_t, 1);
    req->protocol = ROUTE_PROTOCOL_SRV6;
    req->vrf_id = ROUTE_VRF_DEFAULT;
    req->afi = ROUTE_AFI_IPV6;
    req->flags = ROUTE_SUBSCRIBE_FLAG_FULL;

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_SUBSCRIBE, DEV_MODULE_ID_ISIS, DEV_MODULE_ID_ROUTE,
                                                    0, req, sizeof(*req), g_free);
    if (!msg)
    {
        g_free(req);
        return ERRCODE_FAIL;
    }
    int rc = dev_ipc_send(isis_local_ipc_ctx(), DEV_MODULE_ID_ROUTE, msg);
    dev_ipc_message_free(msg);
    if (rc != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: failed to subscribe to SRv6 locator routes");
        return ERRCODE_FAIL;
    }
    LOG_INFO("ISIS: subscribed to local SRv6 locator routes (IPv6, public VRF)");
    return ERRCODE_SUCCESS;
}

static gboolean isis_srv6_locator_entry_valid(const route_msg_entry_t *entry)
{
    if (!entry || entry->protocol != ROUTE_PROTOCOL_SRV6 || entry->vrf_id != ROUTE_VRF_DEFAULT ||
        entry->afi != ROUTE_AFI_IPV6 || entry->safi != ROUTE_SAFI_UNICAST || entry->prefix_len > 127u ||
        entry->prefix_addr.family != AF_INET6 || entry->source_addr.family != AF_INET6 ||
        entry->nh_type != ROUTE_NH_TYPE_BLACKHOLE || entry->source_name[0] == '\0' ||
        memchr(entry->source_name, '\0', sizeof(entry->source_name)) == NULL ||
        !net_addr_equal(&entry->source_addr, &entry->prefix_addr))
    {
        return FALSE;
    }
    net_addr_t normalized = entry->prefix_addr;
    return net_addr_prefix_normalize(&normalized, entry->prefix_len) == 0 &&
           net_addr_equal(&normalized, &entry->prefix_addr);
}

static void isis_srv6_locator_apply_entry(const route_msg_entry_t *entry)
{
    if (!g_isis_work_local || !g_isis_work_local->srv6_locators || !isis_srv6_locator_entry_valid(entry))
    {
        return;
    }

    char prefix[INET6_ADDRSTRLEN];
    net_addr_to_str(&entry->prefix_addr, prefix, sizeof(prefix));
    const char *name = entry->source_name;

    gboolean changed = FALSE;
    isis_srv6_locator_prefix_t *current = g_hash_table_lookup(g_isis_work_local->srv6_locators, name);
    if (entry->is_withdraw)
    {
        /* locator 更新按“先加新前缀、再撤旧前缀”到达。旧前缀的 withdraw
         * 不能误删同名 locator 已经换成的新映射。 */
        if (current && current->prefix_len == entry->prefix_len &&
            net_addr_equal(&current->prefix, &entry->prefix_addr))
        {
            changed = g_hash_table_remove(g_isis_work_local->srv6_locators, name);
        }
    }
    else if (!current || current->prefix_len != entry->prefix_len ||
             !net_addr_equal(&current->prefix, &entry->prefix_addr))
    {
        isis_srv6_locator_prefix_t *locator = g_new0(isis_srv6_locator_prefix_t, 1);
        locator->prefix = entry->prefix_addr;
        locator->prefix_len = entry->prefix_len;
        g_hash_table_replace(g_isis_work_local->srv6_locators, g_strdup(name), locator);
        changed = TRUE;
    }

    if (changed)
    {
        LOG_INFO("ISIS: %s SRv6 locator %s reachability %s/%u", entry->is_withdraw ? "withdrew" : "learned", name,
                 prefix, (unsigned)entry->prefix_len);
        isis_srv6_locator_mark_lsp_dirty(name);
    }
}

static void isis_srv6_locator_handle_route_msg(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->src_module_id != DEV_MODULE_ID_ROUTE || !g_isis_work_local ||
        !g_isis_work_local->route_ready)
    {
        return;
    }
    if (msg->msg_type == ROUTE_MSG_TYPE_UPDATE)
    {
        if (msg->payload_len >= sizeof(route_msg_entry_t))
        {
            isis_srv6_locator_apply_entry((const route_msg_entry_t *)msg->payload);
        }
        return;
    }
    if (msg->msg_type != ROUTE_MSG_TYPE_REPORT || msg->payload_len < sizeof(route_msg_report_t))
    {
        return;
    }

    const route_msg_report_t *report = msg->payload;
    size_t available = (msg->payload_len - sizeof(*report)) / sizeof(route_msg_entry_t);
    if (report->protocol != ROUTE_PROTOCOL_SRV6 || (size_t)report->route_count > available)
    {
        LOG_WARN("ISIS: invalid SRv6 locator route report (protocol=%u count=%u available=%zu)", report->protocol,
                 report->route_count, available);
        return;
    }
    for (uint32_t i = 0u; i < report->route_count; ++i)
    {
        isis_srv6_locator_apply_entry(&report->routes[i]);
    }
}

gboolean isis_if_entry_matches_instance(const isis_instance_cfg_t *inst, const if_api_cache_entry_t *entry)
{
    const char *expected = (inst && inst->vrf_name[0] != '\0') ? inst->vrf_name : "public";
    const char *actual = (entry && entry->vrf_name[0] != '\0') ? entry->vrf_name : "public";
    return inst && entry && strcmp(expected, actual) == 0;
}

static void isis_if_cfg_free(gpointer data)
{
    g_free(data);
}

static void isis_route_state_free(gpointer data)
{
    isis_route_state_t *state = (isis_route_state_t *)data;
    if (state)
    {
        isis_route_state_reset(state);
    }
    g_free(state);
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
    if (inst->nexthop_v4)
    {
        isis_nexthop_table_destroy(inst->nexthop_v4);
        inst->nexthop_v4 = NULL;
    }
    if (inst->nexthop_v6)
    {
        isis_nexthop_table_destroy(inst->nexthop_v6);
        inst->nexthop_v6 = NULL;
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
    g_strlcpy(inst->vrf_name, "public", sizeof(inst->vrf_name));
    inst->is_type = ISIS_IS_TYPE_LEVEL_1_2;
    inst->admin_up = 1u;
    inst->af_ipv4 = 1u;
    inst->af_ipv6 = 1u;
    inst->cost_style = ISIS_DEFAULT_COST_STYLE;
    inst->nexthop_v4 = isis_nexthop_table_create();
    inst->nexthop_v6 = isis_nexthop_table_create();
    if (!inst->nexthop_v4 || !inst->nexthop_v6)
    {
        isis_instance_cfg_free(inst);
        return NULL;
    }
    inst->if_cfgs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_if_cfg_free);
    if (!inst->if_cfgs)
    {
        isis_instance_cfg_free(inst);
        return NULL;
    }
    inst->route_states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_route_state_free);
    if (!inst->route_states)
    {
        isis_instance_cfg_free(inst);
        return NULL;
    }
    inst->learned_route_heads = isis_route_head_table_new();
    if (!inst->learned_route_heads)
    {
        isis_instance_cfg_free(inst);
        return NULL;
    }
    inst->neighbors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_neighbor_free);
    if (!inst->neighbors)
    {
        isis_instance_cfg_free(inst);
        return NULL;
    }

    inst->lsdb_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_lsdb_entry_free);
    if (!inst->lsdb_entries)
    {
        isis_instance_cfg_free(inst);
        return NULL;
    }
    return inst;
}

isis_instance_cfg_t *isis_lookup_instance(uint32_t tag)
{
    if (!g_isis_work_local || !g_isis_work_local->instances || tag == 0u)
    {
        return NULL;
    }
    return (isis_instance_cfg_t *)g_hash_table_lookup(g_isis_work_local->instances, GUINT_TO_POINTER(tag));
}

isis_instance_cfg_t *isis_get_or_create_instance(uint32_t tag)
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

    apply->rc = ISIS_APPLY_RC_FAIL;
    apply->errmsg[0] = '\0';

    switch (apply->op)
    {
        case ISIS_APPLY_OP_INSTANCE_SET:
            isis_cfg_apply_instance_set(apply);
            break;
        case ISIS_APPLY_OP_INSTANCE_DEL:
            isis_cfg_apply_instance_del(apply);
            break;
        case ISIS_APPLY_OP_NET_SET:
            isis_cfg_apply_net_set(apply);
            break;
        case ISIS_APPLY_OP_IS_TYPE_SET:
            isis_cfg_apply_is_type_set(apply);
            break;
        case ISIS_APPLY_OP_COST_STYLE_SET:
            isis_cfg_apply_cost_style_set(apply);
            break;
        case ISIS_APPLY_OP_AF_SET:
            isis_cfg_apply_af_set(apply);
            break;
        case ISIS_APPLY_OP_AF_DEL:
            isis_cfg_apply_af_del(apply);
            break;
        case ISIS_APPLY_OP_IF_SET:
            isis_cfg_apply_if_set(apply);
            break;
        case ISIS_APPLY_OP_IF_DEL:
            isis_cfg_apply_if_del(apply);
            break;
        case ISIS_APPLY_OP_SRV6_LOCATOR_SET:
            isis_cfg_apply_srv6_locator_set(apply);
            break;
        default:
            g_snprintf(apply->errmsg, sizeof(apply->errmsg), "ISIS Error: Unknown apply op %d", apply->op);
            apply->rc = ISIS_APPLY_RC_FAIL;
            break;
    }

    /* worker_dispatch_apply 调用方只关心 apply->rc；本函数返回值仅指示派发完成 */
    return ERRCODE_SUCCESS;
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
                /* worker_apply_cmd 通过 cfg_apply 内部填充 apply->rc / apply->errmsg；
                 * 这里不要用其返回值覆盖 apply->rc */
                (void)worker_apply_cmd(cmd->apply);
            }
            worker_cmd_complete(cmd, ERRCODE_SUCCESS);
            return 0;

        case ISIS_WORKER_CMD_ROUTE_READY:
            if (dev_ipc_wait_connected(isis_local_ipc_ctx(), DEV_MODULE_ID_ROUTE, DEV_IPC_WAIT_PEER_MS) !=
                ERRCODE_SUCCESS)
            {
                LOG_WARN("ISIS: ROUTE connection not ready in time; route replay deferred to next READY");
                break;
            }
            g_isis_work_local->route_ready = TRUE;
            /* ROUTE 重启后旧快照已无效；先清理，再用 protocol=SRV6 的 FULL
             * 订阅重建。locator owner 的重放与订阅存在先后竞态也安全：
             * 先注入会进入 FULL，后注入会作为 UPDATE 到达。 */
            isis_srv6_locator_clear();
            (void)isis_srv6_locator_subscribe();
            /* 先按原 id 反刷 nexthop 对象（ROUTE 重建），再对账/重放路由（复用同一 id） */
            isis_nexthop_resync_all_instances();
            isis_route_sync_reconcile_all_instances();
            isis_route_sync_replay_all_instances();
            break;

        case ISIS_WORKER_CMD_ROUTE_DOWN:
            g_isis_work_local->route_ready = FALSE;
            isis_srv6_locator_clear();
            break;

        case ISIS_WORKER_CMD_ROUTE_MSG:
            isis_srv6_locator_handle_route_msg(cmd->msg);
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            break;

        case ISIS_WORKER_CMD_IF_DOWN:
            /* IF 模块下线（崩溃/被停）：
             *   1) 清掉 IF 共享缓存——所有 isis_neighbor_should_remove 会立即判 true。
             *   2) reconcile_all 触发邻接逐个 withdraw 学到的 LSP、撤 SPF 路由、删邻接表项。
             *   3) reconcile route_sync：把所有 instance 的 route_state 与 ROUTE 模块对账，
             *      由于第 2 步已清空 SPF，结果是把残留下发的路由一次性 withdraw。
             * 此后 IF READY 再到时，if_api_subscribe_all 会把新一轮 IF 状态推回来。 */
            LOG_INFO("ISIS: IF DOWN detected, flushing IF cache + tearing down all adjacencies and routes");
            if_api_cache_cleanup();
            if_api_cache_init();
            isis_neighbor_reconcile_all();
            isis_route_sync_reconcile_all_instances();
            break;

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
    g_isis_work_local->srv6_locators = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!g_isis_work_local->srv6_locators)
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

int isis_worker_post_route_ready(void)
{
    isis_worker_cmd_t *cmd = worker_cmd_create(ISIS_WORKER_CMD_ROUTE_READY, NULL, 0);
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

int isis_worker_post_route_down(void)
{
    isis_worker_cmd_t *cmd = worker_cmd_create(ISIS_WORKER_CMD_ROUTE_DOWN, NULL, 0);
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

int isis_worker_post_route_msg(dev_ipc_message_t *msg)
{
    isis_worker_cmd_t *cmd = worker_cmd_create(ISIS_WORKER_CMD_ROUTE_MSG, msg, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (worker_cmd_enqueue(cmd) != 0)
    {
        cmd->msg = NULL;
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int isis_worker_post_if_down(void)
{
    isis_worker_cmd_t *cmd = worker_cmd_create(ISIS_WORKER_CMD_IF_DOWN, NULL, 0);
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

    if (g_isis_work_local->srv6_locators)
    {
        g_hash_table_destroy(g_isis_work_local->srv6_locators);
        g_isis_work_local->srv6_locators = NULL;
    }

    if_api_cache_cleanup();

    g_free(g_isis_work_local);
    g_isis_work_local = NULL;
}
