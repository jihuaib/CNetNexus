/**
 * @file   if_worker.c
 * @brief  IF worker 线程实现：串行处理 IPC 业务与链路监控事件
 * @author jhb
 * @date   2026/04/21
 */
#include "if_worker.h"

#include <glib.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "if.h"
#include "if_cfg_apply.h"
#include "if_link_monitor.h"
#include "if_main.h"
#include "if_map.h"
#include "if_msg.h"
#include "if_pub.h"
#include "if_show.h"
#include "log.h"
#include "path_utils.h"
#include "route.h"
#include "vrf.h"

if_work_local_t *g_if_work_local = NULL;

/**
 * @brief worker 命令类型
 */
typedef enum if_worker_cmd_type
{
    IF_WORKER_CMD_IPC_MSG = 1,              /**< 通用 IPC 消息（按 msg_type 二次分发） */
    IF_WORKER_CMD_LINK_EVENT = 2,           /**< 链路监控事件 */
    IF_WORKER_CMD_ADDR_EVENT = 3,           /**< 地址监控事件 */
    IF_WORKER_CMD_SHUTDOWN = 4,             /**< 停止 worker 线程 */
    IF_WORKER_CMD_APPLY = 5,                /**< 配置应用命令（waitable） */
    IF_WORKER_CMD_VRF_EVENT = 6,            /**< VRF 事件：维护 worker 独占 VRF cache */
    IF_WORKER_CMD_VRF_QUERY = 7,            /**< VRF 查询：其他线程同步请求 worker 查询 */
    IF_WORKER_CMD_PRE_SHUTDOWN_CLEANUP = 8, /**< 优雅停止前清除所有运行态 IP（不动 DB） */
    IF_WORKER_CMD_ROUTE_READY = 9,          /**< ROUTE ready/restart 后重刷 connected 路由 */
    IF_WORKER_CMD_MODULE_DOWN = 10,         /**< 对端模块 IPC 断开，清理运行态订阅 */
    IF_WORKER_CMD_VRF_DOWN = 11,            /**< VRF 模块 DOWN：清接口 VRF 绑定 + 清 cache */
    IF_WORKER_CMD_RESTORE_DONE = 12,        /**< DB restore 完成：flush pending REPLAY */
    IF_WORKER_CMD_SNMP_REFRESH = 13,        /**< SNMP READY 后重刷标准 IF-MIB */
} if_worker_cmd_type_t;

/**
 * @brief worker 内部命令结构
 */
typedef struct if_worker_cmd
{
    if_worker_cmd_type_t type;
    dev_ipc_message_t *msg;        /**< IPC 消息（IPC_MSG 使用） */
    if_work_link_event_t link_evt; /**< 链路事件（LINK_EVENT 使用） */
    if_work_addr_event_t addr_evt; /**< 地址事件（ADDR_EVENT 使用） */
    if_apply_cmd_t *apply;         /**< 应用命令（APPLY 使用，借用引用） */
    char vrf_name[IF_VRF_NAME_MAX];
    uint32_t *vrf_id_out;
    uint32_t module_id;

    int waitable;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int rc;
} if_worker_cmd_t;

static if_worker_cmd_t *worker_cmd_create(if_worker_cmd_type_t type, dev_ipc_message_t *msg, int waitable)
{
    if_worker_cmd_t *cmd = g_malloc0(sizeof(*cmd));
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

static void worker_cmd_destroy(if_worker_cmd_t *cmd)
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

static void worker_cmd_complete(if_worker_cmd_t *cmd, int rc)
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

static int worker_cmd_wait(if_worker_cmd_t *cmd)
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

static int worker_cmd_enqueue(if_worker_cmd_t *cmd)
{
    if (!cmd || !g_if_work_local || !g_if_work_local->cmd_queue || !g_if_work_local->running)
    {
        return ERRCODE_FAIL;
    }
    g_async_queue_push(g_if_work_local->cmd_queue, cmd);
    return ERRCODE_SUCCESS;
}

static int worker_apply_cmd(if_apply_cmd_t *apply)
{
    if (!apply)
    {
        return ERRCODE_FAIL;
    }

    switch (apply->op)
    {
        case IF_APPLY_OP_IP_SET:
            return if_cfg_apply_ip(apply->u.ip_set.is_no, apply->u.ip_set.ifname, &apply->u.ip_set.prefix);
        case IF_APPLY_OP_SHUTDOWN_SET:
            return if_cfg_apply_shutdown(apply->u.shutdown_set.is_no, apply->u.shutdown_set.ifname);
        case IF_APPLY_OP_LOOP_CREATE:
            return if_cfg_loop_create(apply->u.loop_create.loop_id);
        case IF_APPLY_OP_LOOP_DELETE:
            return if_cfg_loop_delete(apply->u.loop_delete.loop_id);
        case IF_APPLY_OP_VRF_BIND:
            return if_cfg_apply_vrf_binding(apply->u.vrf_bind.ifname, apply->u.vrf_bind.vrf_name);
        default:
            return ERRCODE_FAIL;
    }
}

static int worker_resolve_vrf_id_by_name(const char *vrf_name, uint32_t *vrf_id)
{
    if (!vrf_id)
    {
        return ERRCODE_FAIL;
    }

    *vrf_id = ROUTE_VRF_DEFAULT;
    if (!vrf_name || vrf_name[0] == '\0' || strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        return ERRCODE_SUCCESS;
    }

    const vrf_api_cache_entry_t *vrf = vrf_api_cache_lookup_by_name(vrf_name);
    if (!vrf)
    {
        return ERRCODE_DEP_MISSING;
    }

    *vrf_id = vrf->vrf_id;
    return ERRCODE_SUCCESS;
}

/* g_tree_foreach 收集名字的回调 */
typedef struct
{
    GList *names;
} collect_names_ctx_t;

static gboolean collect_named_with_ip_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    if_map_entry_t *e = (if_map_entry_t *)value;
    collect_names_ctx_t *ctx = (collect_names_ctx_t *)data;
    if (net_prefix_is_set(&e->prefix_v4) || net_prefix_is_set(&e->prefix_v6))
    {
        ctx->names = g_list_append(ctx->names, g_strdup(e->logical_name));
    }
    return FALSE;
}

/**
 * 优雅停止前清理运行态：遍历所有接口，对配置了 IP 的逐个 if_cfg_apply_ip(is_no=1)：
 * - 移除 OS netlink IP（kernel 自动撤销直连路由）
 * - if_pub_notify(PROTO_DOWN) 通知 ROUTE 撤路由
 * 不写 DB → process start 后 db_restore 仍能完整还原。
 */
static int worker_pre_shutdown_cleanup(void)
{
    if (!g_if_work_local || !g_if_work_local->interface_map.all_entries)
    {
        return ERRCODE_SUCCESS;
    }

    collect_names_ctx_t ctx = {NULL};
    g_tree_foreach(g_if_work_local->interface_map.all_entries, collect_named_with_ip_cb, &ctx);

    int removed = 0;
    for (GList *l = ctx.names; l != NULL; l = l->next)
    {
        const char *name = (const char *)l->data;
        if (if_cfg_apply_ip(TRUE, name, NULL) == ERRCODE_SUCCESS)
        {
            removed++;
        }
    }
    g_list_free_full(ctx.names, g_free);
    LOG_INFO("IF: pre-shutdown cleanup removed IPs from %d interface(s)", removed);
    return ERRCODE_SUCCESS;
}

static int worker_remove_subscribers_by_module(uint32_t module_id)
{
    int removed = 0;
    GList *l = g_if_work_local ? g_if_work_local->subscribers : NULL;

    while (l)
    {
        if_subscriber_t *sub = (if_subscriber_t *)l->data;
        GList *next = l->next;

        if (sub && sub->module_id == module_id)
        {
            g_if_work_local->subscribers = g_list_delete_link(g_if_work_local->subscribers, l);
            g_free(sub);
            removed++;
        }

        l = next;
    }

    if (removed > 0)
    {
        LOG_INFO("IF: module 0x%08X down, removed %d subscriber(s)", module_id, removed);
    }
    else
    {
        LOG_DEBUG("IF: module 0x%08X down, no subscriber to remove", module_id);
    }

    return removed;
}

static uint32_t worker_if_type_to_mask(if_type_t type)
{
    switch (type)
    {
        case IF_TYPE_ETHERNET:
        case IF_TYPE_VETH:
            return IF_INTF_TYPE_ETH;
        default:
            return 0u;
    }
}

static gboolean worker_prefix_equal(const net_prefix_t *a, const net_prefix_t *b)
{
    if (!a || !b)
    {
        return FALSE;
    }
    return (a->prefix_len == b->prefix_len && net_addr_equal(&a->addr, &b->addr)) ? TRUE : FALSE;
}

/**
 * @brief 在 worker 线程处理一条 IPC 消息（按 msg_type 二次分发）
 */
static void worker_handle_ipc_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case CLI_MSG_TYPE:
            /* show 命令（IPC 线程已过滤非 show 的配置命令，这里只剩 show） */
            (void)if_show_handle_cli(msg);
            dev_ipc_message_free(msg);
            return;

        case CLI_MSG_TYPE_CONTINUE:
            (void)if_show_handle_continue(msg);
            dev_ipc_message_free(msg);
            return;

        case CLI_MSG_TYPE_QUERY_CANDIDATES:
            if_show_handle_query_candidates(msg);
            /* 响应发送完毕后 handler 内部释放 msg */
            return;

        case IF_MSG_TYPE_SUBSCRIBE:
            if_msg_handle_subscribe(msg);
            return;

        case IF_MSG_TYPE_UNSUBSCRIBE:
            if_msg_handle_unsubscribe(msg);
            return;

        default:
            LOG_WARN("IF-WORKER: unexpected msg type 0x%08X", msg->msg_type);
            dev_ipc_message_free(msg);
            return;
    }
}

static void *if_worker_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "if-worker");
    log_set_tag("if");
    vrf_api_cache_init();

    while (g_if_work_local && g_if_work_local->running)
    {
        if_worker_cmd_t *cmd = (if_worker_cmd_t *)g_async_queue_pop(g_if_work_local->cmd_queue);
        if (!cmd)
        {
            continue;
        }

        switch (cmd->type)
        {
            case IF_WORKER_CMD_IPC_MSG:
                worker_handle_ipc_msg(cmd->msg);
                cmd->msg = NULL;
                break;

            case IF_WORKER_CMD_LINK_EVENT:
                if_link_monitor_handle_work_event(&cmd->link_evt);
                break;

            case IF_WORKER_CMD_ADDR_EVENT:
                if_link_monitor_handle_addr_work_event(&cmd->addr_evt);
                break;

            case IF_WORKER_CMD_SHUTDOWN:
                g_if_work_local->running = 0;
                break;

            case IF_WORKER_CMD_APPLY:
                if (cmd->apply)
                {
                    cmd->apply->rc = worker_apply_cmd(cmd->apply);
                    worker_cmd_complete(cmd, cmd->apply->rc);
                    /* waitable 命令由派发方在 worker_cmd_wait 返回后释放 */
                    continue;
                }
                worker_cmd_complete(cmd, ERRCODE_FAIL);
                /* waitable 命令由派发方在 worker_cmd_wait 返回后释放 */
                continue;

            case IF_WORKER_CMD_VRF_EVENT:
            {
                /* 先解析事件类型 / VRF 名，再更新缓存（缓存更新后 VRF entry 可能被释放）。
                 * 接口业务清理已在 VRF DOWN 路径（IF_WORKER_CMD_VRF_DOWN）完成。 */
                uint32_t vrf_event = 0;
                char vrf_name[VRF_NAME_MAX_LEN] = {0};
                if (cmd->msg && cmd->msg->payload && cmd->msg->payload_len >= offsetof(vrf_event_msg_t, rts))
                {
                    const vrf_event_msg_t *evt = (const vrf_event_msg_t *)cmd->msg->payload;
                    vrf_event = evt->event;
                    g_strlcpy(vrf_name, evt->name, sizeof(vrf_name));
                }

                vrf_api_cache_on_event(cmd->msg);

                /* 级联：VRF 删除时把所有绑定该 VRF 的接口移回 public，
                 * 否则 IF 模块会保留 stale 的 vrf_name 状态，CLI 后续无法 no vrf forwarding。 */
                if (vrf_event == VRF_EVENT_VRF_DEL && vrf_name[0] != '\0')
                {
                    int unbound = if_cfg_apply_vrf_deleted(vrf_name);
                    if (unbound > 0)
                    {
                        LOG_INFO("IF: VRF '%s' deleted, cascaded unbind on %d interface(s)", vrf_name, unbound);
                    }
                }

                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
                worker_cmd_complete(cmd, ERRCODE_SUCCESS);
                continue;
            }

            case IF_WORKER_CMD_VRF_QUERY:
                worker_cmd_complete(cmd, worker_resolve_vrf_id_by_name(cmd->vrf_name, cmd->vrf_id_out));
                continue;

            case IF_WORKER_CMD_PRE_SHUTDOWN_CLEANUP:
                worker_cmd_complete(cmd, worker_pre_shutdown_cleanup());
                continue;

            case IF_WORKER_CMD_ROUTE_READY:
                g_if_work_local->route_ready = 1;
                (void)if_cfg_replay_connected_routes();
                break;

            case IF_WORKER_CMD_MODULE_DOWN:
                if (cmd->module_id == DEV_MODULE_ID_ROUTE)
                {
                    g_if_work_local->route_ready = 0;
                }
                (void)worker_remove_subscribers_by_module(cmd->module_id);
                break;

            case IF_WORKER_CMD_VRF_DOWN:
            {
                int cleared = if_cfg_purge_non_public_vrf_bindings_mem();
                if (cleared > 0)
                {
                    LOG_INFO("IF: VRF down, purged binding on %d interface(s)", cleared);
                }
                vrf_api_cache_clear();
                break;
            }

            case IF_WORKER_CMD_RESTORE_DONE:
                g_if_work_local->restore_done = 1;
                if_msg_flush_pending_replays();
                break;

            case IF_WORKER_CMD_SNMP_REFRESH:
                if_pub_snmp_refresh_all();
                break;

            default:
                LOG_WARN("IF-WORKER: unknown cmd type=%d", (int)cmd->type);
                if (cmd->msg)
                {
                    dev_ipc_message_free(cmd->msg);
                    cmd->msg = NULL;
                }
                break;
        }

        worker_cmd_destroy(cmd);
    }

    vrf_api_cache_cleanup();

    return NULL;
}

/**
 * @brief 解析 GE 接口映射配置文件路径
 */
static char *resolve_if_map_path(void)
{
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir != NULL)
    {
        char *path = g_build_filename(work_dir, "resources", "if", "if_map.conf.gns3", NULL);
        LOG_INFO("Using GNS3 interface mapping: %s", path);
        return path;
    }

    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
    {
        LOG_ERROR("Failed to get exe directory");
        return NULL;
    }

    char *path = g_build_filename(exe_dir, "..", "..", "src", "if", "resources", "if_map.conf.local", NULL);
    LOG_INFO("Using local interface mapping: %s", path);
    return path;
}

int if_worker_prepare(void)
{
    if (g_if_work_local)
    {
        return ERRCODE_SUCCESS;
    }

    g_if_work_local = g_malloc0(sizeof(*g_if_work_local));
    if (!g_if_work_local)
    {
        return ERRCODE_FAIL;
    }

    g_if_work_local->cmd_queue = g_async_queue_new();
    if (!g_if_work_local->cmd_queue)
    {
        g_free(g_if_work_local);
        g_if_work_local = NULL;
        return ERRCODE_FAIL;
    }

    /* 加载 GE 接口映射；interface_map 属于 worker 独占资源 */
    char *map_path = resolve_if_map_path();
    if (map_path)
    {
        if (if_map_init(map_path) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("Failed to load interface mapping");
        }
        g_free(map_path);
    }

    return ERRCODE_SUCCESS;
}

int if_worker_launch(void)
{
    if (!g_if_work_local || !g_if_work_local->cmd_queue)
    {
        return ERRCODE_FAIL;
    }

    if (g_if_work_local->running)
    {
        return ERRCODE_SUCCESS;
    }

    g_if_work_local->running = 1;
    if (pthread_create(&g_if_work_local->thread, NULL, if_worker_thread_fn, NULL) != 0)
    {
        g_if_work_local->running = 0;
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int if_worker_is_running(void)
{
    return (g_if_work_local && g_if_work_local->running && g_if_work_local->thread != 0) ? 1 : 0;
}

int if_worker_post_ipc_message(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_IPC_MSG, msg, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }

    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        cmd->msg = NULL;
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int if_worker_dispatch_vrf_event(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_VRF_EVENT, msg, 1);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }

    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        cmd->msg = NULL;
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }

    int rc = worker_cmd_wait(cmd);
    worker_cmd_destroy(cmd);
    return rc;
}

int if_worker_pre_shutdown_cleanup(void)
{
    if (!g_if_work_local || !g_if_work_local->running || g_if_work_local->thread == 0)
    {
        return ERRCODE_SUCCESS;
    }
    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_PRE_SHUTDOWN_CLEANUP, NULL, 1);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    int rc = worker_cmd_wait(cmd);
    worker_cmd_destroy(cmd);
    return rc;
}

int if_worker_post_route_ready(void)
{
    if (!g_if_work_local || !g_if_work_local->running || g_if_work_local->thread == 0)
    {
        return ERRCODE_SUCCESS;
    }
    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_ROUTE_READY, NULL, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int if_worker_post_restore_done(void)
{
    if (!g_if_work_local || !g_if_work_local->running || g_if_work_local->thread == 0)
    {
        return ERRCODE_SUCCESS;
    }
    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_RESTORE_DONE, NULL, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int if_worker_post_snmp_refresh(void)
{
    if (!g_if_work_local || !g_if_work_local->running || g_if_work_local->thread == 0)
    {
        return ERRCODE_SUCCESS;
    }
    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_SNMP_REFRESH, NULL, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int if_worker_is_restore_done(void)
{
    return (g_if_work_local && g_if_work_local->restore_done) ? 1 : 0;
}

int if_worker_post_vrf_down(void)
{
    if (!g_if_work_local || !g_if_work_local->running || g_if_work_local->thread == 0)
    {
        return ERRCODE_SUCCESS;
    }
    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_VRF_DOWN, NULL, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int if_worker_post_module_down(uint32_t module_id)
{
    if (!g_if_work_local || !g_if_work_local->running || g_if_work_local->thread == 0 || module_id == 0)
    {
        return ERRCODE_FAIL;
    }
    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_MODULE_DOWN, NULL, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    cmd->module_id = module_id;
    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int if_worker_dispatch_apply(if_apply_cmd_t *apply)
{
    if (!apply)
    {
        return ERRCODE_FAIL;
    }

    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_APPLY, NULL, 1);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    cmd->apply = apply;

    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }

    int rc = worker_cmd_wait(cmd);
    worker_cmd_destroy(cmd);
    return rc;
}

int if_worker_resolve_vrf_id_by_name(const char *vrf_name, uint32_t *vrf_id)
{
    if (!vrf_id)
    {
        return ERRCODE_FAIL;
    }

    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_VRF_QUERY, NULL, 1);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (vrf_name)
    {
        g_strlcpy(cmd->vrf_name, vrf_name, sizeof(cmd->vrf_name));
    }
    cmd->vrf_id_out = vrf_id;

    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }

    int rc = worker_cmd_wait(cmd);
    worker_cmd_destroy(cmd);
    return rc;
}

int if_worker_post_link_event(uint16_t nlmsg_type, const char *physical_name, uint32_t ifindex, uint8_t link_up_known,
                              uint8_t link_up)
{
    if (!physical_name || physical_name[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_LINK_EVENT, NULL, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }

    cmd->link_evt.nlmsg_type = nlmsg_type;
    cmd->link_evt.ifindex = ifindex;
    cmd->link_evt.link_up_known = link_up_known ? 1u : 0u;
    cmd->link_evt.link_up = link_up ? 1u : 0u;
    g_strlcpy(cmd->link_evt.physical_name, physical_name, sizeof(cmd->link_evt.physical_name));

    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int if_worker_post_addr_event(uint16_t nlmsg_type, const char *physical_name, uint32_t ifindex, uint32_t addr_flags,
                              const net_prefix_t *prefix)
{
    if (!physical_name || physical_name[0] == '\0' || !prefix || !net_prefix_is_set(prefix))
    {
        return ERRCODE_FAIL;
    }

    if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_ADDR_EVENT, NULL, 0);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }

    cmd->addr_evt.nlmsg_type = nlmsg_type;
    cmd->addr_evt.ifindex = ifindex;
    cmd->addr_evt.addr_flags = addr_flags;
    cmd->addr_evt.prefix = *prefix;
    g_strlcpy(cmd->addr_evt.physical_name, physical_name, sizeof(cmd->addr_evt.physical_name));

    if (worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

void if_worker_shutdown(void)
{
    if (!g_if_work_local)
    {
        return;
    }

    if (g_if_work_local->running && g_if_work_local->thread != 0)
    {
        if_worker_cmd_t *cmd = worker_cmd_create(IF_WORKER_CMD_SHUTDOWN, NULL, 0);
        if (cmd)
        {
            g_async_queue_push(g_if_work_local->cmd_queue, cmd);
        }

        (void)pthread_join(g_if_work_local->thread, NULL);
        g_if_work_local->thread = 0;
        g_if_work_local->running = 0;
    }

    if (g_if_work_local->cmd_queue)
    {
        if_worker_cmd_t *cmd = NULL;
        while ((cmd = (if_worker_cmd_t *)g_async_queue_try_pop(g_if_work_local->cmd_queue)) != NULL)
        {
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            if (cmd->type == IF_WORKER_CMD_VRF_EVENT && cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
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
        g_async_queue_unref(g_if_work_local->cmd_queue);
        g_if_work_local->cmd_queue = NULL;
    }

    /* 释放业务数据 */
    if_show_cleanup_state();
    g_list_free_full(g_if_work_local->subscribers, g_free);
    g_if_work_local->subscribers = NULL;
    if (g_if_work_local->interface_map.all_entries)
    {
        g_tree_destroy(g_if_work_local->interface_map.all_entries);
        g_if_work_local->interface_map.all_entries = NULL;
    }

    g_free(g_if_work_local);
    g_if_work_local = NULL;
}

/* ============================================================================
 * 辅助：按 physical_name 查找接口条目
 * ============================================================================ */

typedef struct
{
    const char *physical_name;
    if_map_entry_t *result;
} find_by_phys_ctx_t;

static gboolean find_by_phys_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    if_map_entry_t *e = (if_map_entry_t *)value;
    find_by_phys_ctx_t *ctx = (find_by_phys_ctx_t *)user_data;
    if (strcmp(e->physical_name, ctx->physical_name) == 0)
    {
        ctx->result = e;
        return TRUE; /* 找到，停止遍历 */
    }
    return FALSE;
}

static if_map_entry_t *find_entry_by_physical(const char *physical_name)
{
    if (!physical_name || !g_if_work_local || !g_if_work_local->interface_map.all_entries)
    {
        return NULL;
    }
    find_by_phys_ctx_t ctx = {.physical_name = physical_name, .result = NULL};
    g_tree_foreach(g_if_work_local->interface_map.all_entries, find_by_phys_cb, &ctx);
    return ctx.result;
}

/* ============================================================================
 * IF work 线程中的链路事件处理
 * ============================================================================ */

/**
 * @brief 处理接口删除事件（RTM_DELLINK）
 */
static void handle_link_del(const if_work_link_event_t *evt)
{
    const char *ifname = evt->physical_name;
    if_map_entry_t *entry = find_entry_by_physical(ifname);
    if (!entry)
    {
        return;
    }

    /* 快照并更新 */
    uint32_t old_ifindex = entry->ifindex;
    int old_link_up = (entry->link_up >= 0) ? entry->link_up : ((old_ifindex != 0u) ? 1 : 0);
    char logical[LOGICAL_NAME_LEN];
    g_strlcpy(logical, entry->logical_name, sizeof(logical));
    int shutdown = entry->shutdown;
    net_prefix_t pfx_v4 = entry->prefix_v4;
    net_prefix_t pfx_v6 = entry->prefix_v6;

    entry->ifindex = 0u;
    entry->link_up = 0;
    memset(&entry->prefix_v6_linklocal, 0, sizeof(entry->prefix_v6_linklocal));

    LOG_INFO("IF-MONITOR: interface %s(%s) deleted, old ifindex=%u", logical, ifname, old_ifindex);

    if (shutdown || old_link_up == 0 || old_ifindex == 0u)
    {
        return; /* 管理状态 DOWN，无需撤销 */
    }

    /* 撤销直连路由（使用保存的旧 ifindex） */
    if_cfg_handle_link_down(logical, old_ifindex, &pfx_v4, &pfx_v6);

    /* 发布事件 */
    entry = find_entry_by_physical(ifname);
    if (entry)
    {
        if (net_prefix_is_set(&pfx_v4))
        {
            if_pub_notify(g_if_work_local->subscribers, entry, IF_INTF_TYPE_ETH, IF_EVENT_PROTO_DOWN, 0, &pfx_v4,
                          old_ifindex);
        }
        if (net_prefix_is_set(&pfx_v6))
        {
            if_pub_notify(g_if_work_local->subscribers, entry, IF_INTF_TYPE_ETH, IF_EVENT_PROTO_DOWN, 0, &pfx_v6,
                          old_ifindex);
        }
        if_pub_notify(g_if_work_local->subscribers, entry, IF_INTF_TYPE_ETH, IF_EVENT_LINK_DOWN, 0, NULL, 0);
    }
}

/**
 * @brief 处理接口出现/状态变化事件（RTM_NEWLINK）
 */
static void handle_link_new(const if_work_link_event_t *evt)
{
    const char *ifname = evt->physical_name;
    uint32_t new_ifindex = evt->ifindex;
    if_map_entry_t *entry = find_entry_by_physical(ifname);
    if (!entry)
    {
        return;
    }

    uint32_t old_ifindex = entry->ifindex;
    int old_link_up = (entry->link_up >= 0) ? entry->link_up : ((old_ifindex != 0u) ? 1 : 0);
    int new_link_up = old_link_up;
    if (evt->link_up_known)
    {
        new_link_up = evt->link_up ? 1 : 0;
    }
    else if (new_ifindex == 0u)
    {
        new_link_up = 0;
    }

    /* 更新 ifindex */
    entry->ifindex = new_ifindex;
    entry->link_up = new_link_up;

    /* 快照必要字段 */
    char logical[LOGICAL_NAME_LEN];
    g_strlcpy(logical, entry->logical_name, sizeof(logical));
    int shutdown = entry->shutdown;
    net_prefix_t pfx_v4 = entry->prefix_v4;
    net_prefix_t pfx_v6 = entry->prefix_v6;

    gboolean transitioned_down = (old_link_up != 0 && new_link_up == 0);
    gboolean transitioned_up = (old_link_up == 0 && new_link_up != 0);
    gboolean ifindex_replaced_while_up =
        (new_link_up != 0 && old_ifindex != 0u && new_ifindex != 0u && old_ifindex != new_ifindex);
    gboolean recovered_from_missing_ifindex = (new_link_up != 0 && old_ifindex == 0u && new_ifindex != 0u);
    gboolean need_recover = transitioned_up || ifindex_replaced_while_up || recovered_from_missing_ifindex;

    if (!transitioned_down && !need_recover)
    {
        return;
    }

    LOG_INFO("IF-MONITOR: interface %s(%s) update: ifindex %u->%u, link %d->%d, shutdown=%d, link_known=%u", logical,
             ifname, old_ifindex, new_ifindex, old_link_up, new_link_up, shutdown, (unsigned int)evt->link_up_known);

    if (shutdown)
    {
        return; /* 管理状态 DOWN，不做恢复 */
    }

    if (transitioned_down)
    {
        if (old_ifindex != 0u)
        {
            /* 撤销直连路由（使用失链前 ifindex） */
            if_cfg_handle_link_down(logical, old_ifindex, &pfx_v4, &pfx_v6);
        }

        entry = find_entry_by_physical(ifname);
        if (entry)
        {
            if (net_prefix_is_set(&pfx_v4))
            {
                if_pub_notify(g_if_work_local->subscribers, entry, IF_INTF_TYPE_ETH, IF_EVENT_PROTO_DOWN, 0, &pfx_v4,
                              old_ifindex);
            }
            if (net_prefix_is_set(&pfx_v6))
            {
                if_pub_notify(g_if_work_local->subscribers, entry, IF_INTF_TYPE_ETH, IF_EVENT_PROTO_DOWN, 0, &pfx_v6,
                              old_ifindex);
            }
            if_pub_notify(g_if_work_local->subscribers, entry, IF_INTF_TYPE_ETH, IF_EVENT_LINK_DOWN, 0, NULL, 0);
        }
        return;
    }

    if (new_ifindex == 0u)
    {
        LOG_WARN("IF-MONITOR: skip recover for %s because ifindex is 0", logical);
        return;
    }

    /* 恢复阶段（包含同步 RPC） */
    if_cfg_recover_link(logical, new_ifindex, &pfx_v4, &pfx_v6);

    /* 发布事件 */
    entry = find_entry_by_physical(ifname);
    if (entry)
    {
        uint32_t cur_ifindex = entry->ifindex;
        if_pub_notify(g_if_work_local->subscribers, entry, IF_INTF_TYPE_ETH, IF_EVENT_LINK_UP, 1, NULL, 0);
        if (!entry->shutdown)
        {
            if (net_prefix_is_set(&entry->prefix_v4))
            {
                if_pub_notify(g_if_work_local->subscribers, entry, IF_INTF_TYPE_ETH, IF_EVENT_PROTO_UP, 0,
                              &entry->prefix_v4, cur_ifindex);
            }
            if (net_prefix_is_set(&entry->prefix_v6))
            {
                if_pub_notify(g_if_work_local->subscribers, entry, IF_INTF_TYPE_ETH, IF_EVENT_PROTO_UP, 0,
                              &entry->prefix_v6, cur_ifindex);
            }
        }
    }
}

void if_link_monitor_handle_addr_work_event(const if_work_addr_event_t *evt)
{
    if (!evt || evt->physical_name[0] == '\0' || evt->prefix.addr.family != AF_INET6 ||
        (evt->addr_flags & IF_ADDR_FLAG_LINK_LOCAL) == 0u)
    {
        return;
    }

    if_map_entry_t *entry = find_entry_by_physical(evt->physical_name);
    if (!entry)
    {
        return;
    }

    if (evt->ifindex != 0u)
    {
        entry->ifindex = evt->ifindex;
    }

    net_prefix_t old_linklocal = entry->prefix_v6_linklocal;
    gboolean had_old = net_prefix_is_set(&old_linklocal);

    if (evt->nlmsg_type == RTM_NEWADDR)
    {
        if (had_old && worker_prefix_equal(&old_linklocal, &evt->prefix))
        {
            return;
        }
        entry->prefix_v6_linklocal = evt->prefix;
    }
    else if (evt->nlmsg_type == RTM_DELADDR)
    {
        if (!had_old)
        {
            return;
        }
        memset(&entry->prefix_v6_linklocal, 0, sizeof(entry->prefix_v6_linklocal));
    }
    else
    {
        return;
    }

    uint32_t if_type = worker_if_type_to_mask(if_detect_type(entry->physical_name));
    if (if_type == 0u)
    {
        return;
    }

    if (had_old && evt->nlmsg_type == RTM_NEWADDR)
    {
        if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_PROTO_DOWN, 0, &old_linklocal,
                      evt->ifindex);
    }
    if (evt->nlmsg_type == RTM_DELADDR)
    {
        if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_PROTO_DOWN, 0, &old_linklocal,
                      evt->ifindex);
    }
    else
    {
        if_pub_notify(g_if_work_local->subscribers, entry, if_type, IF_EVENT_PROTO_UP, 0, &entry->prefix_v6_linklocal,
                      evt->ifindex);
    }
}

void if_link_monitor_handle_work_event(const if_work_link_event_t *evt)
{
    if (!evt || evt->physical_name[0] == '\0')
    {
        return;
    }

    if (evt->nlmsg_type == RTM_DELLINK)
    {
        handle_link_del(evt);
        return;
    }

    if (evt->nlmsg_type == RTM_NEWLINK)
    {
        handle_link_new(evt);
        return;
    }
}
