/**
 * @file   route_worker.c
 * @brief  Route worker 线程实现：epoll 事件循环与业务数据处理
 * @author jhb
 * @date   2026/03/28
 */
#include "route_worker.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cli.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_bdr.h"
#include "route_calc.h"
#include "route_cfg_apply.h"
#include "route_main.h"
#include "route_pub.h"
#include "route_relay.h"
#include "route_rib.h"
#include "route_show.h"
#include "route_static.h"
#include "route_work.h"

/** worker epoll 单次最大事件数 */
#define ROUTE_MAX_EPOLL_EVENTS 8

route_work_local_t *g_route_work_local = NULL;

/** cmd eventfd 哨兵标签（地址用于 epoll data.ptr 精确比较） */
static char g_route_cmd_tag;
/** work eventfd 哨兵标签（地址用于 epoll data.ptr 精确比较） */
static char g_route_work_tag;

// ============================================================================
// 内部命令队列结构（私有）
// ============================================================================

/**
 * @brief worker 内部命令结构体（IPC 线程分配，worker 线程处理后释放）
 */
typedef struct route_worker_cmd
{
    route_worker_cmd_type_t type; /**< 命令类型 */
    dev_ipc_message_t *msg;       /**< 关联 IPC 消息（可为 NULL） */
    route_apply_cmd_t *apply;     /**< APPLY 命令参数（借用引用，不持有所有权） */
    int waitable;                 /**< 是否需要同步等待结果（APPLY 命令为 1） */
    pthread_mutex_t mutex;        /**< 同步等待互斥锁 */
    pthread_cond_t cond;          /**< 同步等待条件变量 */
    int done;                     /**< worker 是否已完成处理 */
    int rc;                       /**< worker 处理结果 */
} route_worker_cmd_t;

typedef enum route_worker_event_type
{
    ROUTE_WORK_EVENT_CALC = 1, /**< 触发一条前缀的优选处理 */
} route_worker_event_type_t;

typedef struct route_worker_event
{
    route_worker_event_type_t type;
    union
    {
        route_head_key_t key;
    } u;
} route_worker_event_t;

static route_worker_cmd_t *worker_cmd_create(route_worker_cmd_type_t type, dev_ipc_message_t *msg, int waitable)
{
    route_worker_cmd_t *cmd = (route_worker_cmd_t *)g_malloc0(sizeof(route_worker_cmd_t));
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

static void worker_cmd_destroy(route_worker_cmd_t *cmd)
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

static void worker_cmd_complete(route_worker_cmd_t *cmd, int rc)
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

static int worker_cmd_wait(route_worker_cmd_t *cmd)
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

static route_worker_event_t *worker_event_create_calc(const route_head_key_t *key)
{
    if (!key)
    {
        return NULL;
    }

    route_worker_event_t *evt = (route_worker_event_t *)g_malloc(sizeof(*evt));
    if (!evt)
    {
        return NULL;
    }

    evt->type = ROUTE_WORK_EVENT_CALC;
    evt->u.key = *key;
    return evt;
}

static void worker_event_destroy(route_worker_event_t *evt)
{
    if (!evt)
    {
        return;
    }
    g_free(evt);
}

static void worker_signal_cmd_event(void)
{
    if (!g_route_work_local || g_route_work_local->cmd_eventfd < 0)
    {
        return;
    }

    uint64_t one = 1;
    if (write(g_route_work_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("[route_worker] write cmd_eventfd 失败");
        }
    }
}

static void worker_signal_work_event(void)
{
    if (!g_route_work_local || g_route_work_local->work_eventfd < 0)
    {
        return;
    }

    uint64_t one = 1;
    if (write(g_route_work_local->work_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("[route_worker] write work_eventfd 失败");
        }
    }
}

static int worker_cmd_enqueue(route_worker_cmd_t *cmd)
{
    if (!cmd || !g_route_work_local || !g_route_work_local->cmd_queue || g_route_work_local->cmd_eventfd < 0)
    {
        return -1;
    }
    g_async_queue_push(g_route_work_local->cmd_queue, cmd);
    worker_signal_cmd_event();
    return 0;
}

static int worker_event_enqueue(route_worker_event_t *evt)
{
    if (!evt || !g_route_work_local || !g_route_work_local->work_queue || g_route_work_local->work_eventfd < 0)
    {
        return -1;
    }

    g_async_queue_push(g_route_work_local->work_queue, evt);
    worker_signal_work_event();
    return 0;
}

static int route_worker_post_calc_event(const route_head_key_t *key)
{
    route_worker_event_t *evt = worker_event_create_calc(key);
    if (!evt)
    {
        return -1;
    }
    if (worker_event_enqueue(evt) != 0)
    {
        worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

// ============================================================================
// 辅助：路径删除前回调（由 route_rib_del 在删除前触发）
// ============================================================================

/**
 * @brief route_rib_del 回调：在路径从 RIB 移除前同步触发优选重算
 *
 * 此时 del_path 仍在 RIB 中，route_calc_on_path_del 可跳过它选次优路径。
 */
static void on_inject_path_del(const route_head_t *head, const route_path_t *path, void *userdata)
{
    (void)userdata;
    route_calc_on_path_del(head, path);
}

static void worker_send_inject_ack(const dev_ipc_message_t *req, int32_t result)
{
    if (!req || req->request_id == 0)
    {
        return;
    }

    route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
    if (!ack)
    {
        return;
    }
    ack->result = result;

    dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, req->src_module_id,
                                                     req->request_id, ack, sizeof(route_msg_ack_t), g_free);
    if (!resp)
    {
        g_free(ack);
        return;
    }

    dev_ipc_send_response(route_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
}

// ============================================================================
// 命令处理：业务逻辑（在 worker 线程运行）
// ============================================================================

static void worker_handle_inject(dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_msg_entry_t))
    {
        LOG_WARN("[route_worker] INJECT payload 太短: %u", msg->payload_len);
        worker_send_inject_ack(msg, ERRCODE_FAIL);
        dev_ipc_message_free(msg);
        return;
    }

    const route_msg_entry_t *entry = (const route_msg_entry_t *)msg->payload;
    int ret = -1;

    if (entry->is_withdraw)
    {
        ret = route_rib_del(g_route_work_local->rib, entry->vrf_id, entry->afi, &entry->prefix_addr, entry->prefix_len,
                            entry->protocol, &entry->source_addr, on_inject_path_del, NULL);
        LOG_DEBUG("[route_worker] INJECT withdraw: vrf=%u afi=%u pfxlen=%u proto=%u ret=%d", entry->vrf_id, entry->afi,
                  entry->prefix_len, entry->protocol, ret);
        route_recompute_iter_paths();
    }
    else
    {
        ret = route_rib_add(g_route_work_local->rib, entry->vrf_id, entry->afi, &entry->prefix_addr, entry->prefix_len,
                            entry->protocol, &entry->source_addr, &entry->nexthop_addr, entry->metric,
                            entry->preference, entry->out_ifindex);
        if (ret >= 0)
        {
            const route_head_t *head = route_rib_lookup_head(g_route_work_local->rib, entry->vrf_id, entry->afi,
                                                             &entry->prefix_addr, entry->prefix_len);
            if (head)
            {
                const route_path_t *path = route_rib_lookup_path(head, entry->protocol, &entry->source_addr);
                if (path)
                {
                    route_path_t *mut = (route_path_t *)path;
                    if (entry->iter_nexthop_addr.family == AF_INET || entry->iter_nexthop_addr.family == AF_INET6)
                    {
                        mut->relay_addr = entry->iter_nexthop_addr;
                    }
                    else
                    {
                        mut->relay_addr = entry->nexthop_addr;
                    }
                    mut->iter_out_ifindex =
                        (entry->iter_out_ifindex != 0u) ? entry->iter_out_ifindex : entry->out_ifindex;
                }
                if (route_worker_post_calc_event(&head->key) != 0)
                {
                    route_work_handle_calc_event(&head->key);
                }
            }
        }
        route_recompute_iter_paths();
    }

    worker_send_inject_ack(msg, (ret >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL);
    dev_ipc_message_free(msg);
}

static void worker_handle_subscribe(dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_subscribe_req_t))
    {
        LOG_WARN("[route_worker] SUBSCRIBE payload 长度不足: %u", msg->payload_len);
        dev_ipc_message_free(msg);
        return;
    }

    const route_subscribe_req_t *req = (const route_subscribe_req_t *)msg->payload;
    uint32_t protocol = req->protocol;
    uint32_t vrf_id = req->vrf_id;
    uint32_t flags = req->flags;

    for (GList *l = g_route_work_local->subscribers; l; l = l->next)
    {
        route_subscriber_t *sub = (route_subscriber_t *)l->data;
        if (sub->module_id == msg->src_module_id && sub->protocol == protocol && sub->vrf_id == vrf_id)
        {
            LOG_DEBUG("[route_worker] module 0x%08X 重复订阅，忽略", msg->src_module_id);
            if (flags & ROUTE_SUBSCRIBE_FLAG_FULL)
            {
                route_calc_pub_dump(msg->src_module_id, protocol, vrf_id, msg->request_id);
            }
            else
            {
                route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
                ack->result = ERRCODE_SUCCESS;
                dev_ipc_message_t *resp =
                    dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id, msg->request_id,
                                           ack, sizeof(route_msg_ack_t), g_free);
                dev_ipc_send_response(route_local_ipc_ctx(), resp);
                dev_ipc_message_free(resp);
            }
            dev_ipc_message_free(msg);
            return;
        }
    }

    route_subscriber_t *sub = (route_subscriber_t *)g_malloc(sizeof(route_subscriber_t));
    sub->module_id = msg->src_module_id;
    sub->protocol = protocol;
    sub->vrf_id = vrf_id;
    g_route_work_local->subscribers = g_list_append(g_route_work_local->subscribers, sub);

    LOG_INFO("[route_worker] module 0x%08X 订阅路由: protocol=%u vrf=%u flags=0x%X", msg->src_module_id, protocol,
             vrf_id, flags);

    if (flags & ROUTE_SUBSCRIBE_FLAG_FULL)
    {
        route_calc_pub_dump(msg->src_module_id, protocol, vrf_id, msg->request_id);
    }
    else
    {
        route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
        ack->result = ERRCODE_SUCCESS;
        dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                         msg->request_id, ack, sizeof(route_msg_ack_t), g_free);
        dev_ipc_send_response(route_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }

    dev_ipc_message_free(msg);
}

static void worker_handle_unsubscribe(dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_subscribe_req_t))
    {
        dev_ipc_message_free(msg);
        return;
    }

    const route_subscribe_req_t *req = (const route_subscribe_req_t *)msg->payload;
    uint32_t protocol = req->protocol;
    uint32_t vrf_id = req->vrf_id;

    GList *l = g_route_work_local->subscribers;
    while (l)
    {
        route_subscriber_t *sub = (route_subscriber_t *)l->data;
        GList *next = l->next;
        if (sub->module_id == msg->src_module_id && sub->protocol == protocol && sub->vrf_id == vrf_id)
        {
            g_route_work_local->subscribers = g_list_delete_link(g_route_work_local->subscribers, l);
            g_free(sub);
            LOG_INFO("[route_worker] module 0x%08X 取消订阅: protocol=%u vrf=%u", msg->src_module_id, protocol, vrf_id);
            break;
        }
        l = next;
    }

    route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
    ack->result = ERRCODE_SUCCESS;
    dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                     msg->request_id, ack, sizeof(route_msg_ack_t), g_free);
    dev_ipc_send_response(route_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
    dev_ipc_message_free(msg);
}

// ============================================================================
// 命令分发
// ============================================================================

/**
 * @brief 处理单条 worker 命令（在 worker 线程调用）
 * @return 1 表示收到 SHUTDOWN 命令，0 表示正常处理
 */
static int worker_dispatch_cmd(route_worker_cmd_t *cmd)
{
    if (!cmd)
    {
        return 0;
    }

    int stop = 0;

    switch (cmd->type)
    {
        case ROUTE_WORKER_CMD_INJECT:
            worker_handle_inject(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_NH_REGISTER:
            route_relay_handle_nh_register(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_NH_UNREGISTER:
            route_relay_handle_nh_unregister(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_SUBSCRIBE:
            worker_handle_subscribe(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_UNSUBSCRIBE:
            worker_handle_unsubscribe(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_CLI_SHOW:
            /* show 命令和 continue 消息，在 worker 线程中访问内存 RIB */
            route_show_dispatch(cmd->msg);
            dev_ipc_message_free(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_APPLY:
        {
            /* 配置应用：按操作类型分发到对应 apply 函数 */
            route_apply_cmd_t *apply = cmd->apply;
            if (apply)
            {
                switch (apply->op)
                {
                    case ROUTE_APPLY_STATIC_ADD:
                    case ROUTE_APPLY_STATIC_DEL:
                    case ROUTE_APPLY_STATIC_DEL_PREFIX:
                        route_cfg_apply_static(apply);
                        break;
                    case ROUTE_APPLY_BATCH_ADD:
                    case ROUTE_APPLY_BATCH_DEL:
                        route_cfg_apply_batch(apply);
                        break;
                    default:
                        LOG_WARN("[route_worker] 未知 apply op: %d", (int)apply->op);
                        apply->rc = -1;
                        break;
                }
            }
            worker_cmd_complete(cmd, apply ? apply->rc : -1);
            /* waitable cmd 由 IPC 线程在 dispatch_apply 返回后自行销毁 */
            return 0;
        }

        case ROUTE_WORKER_CMD_IF_EVENT:
            /* IF 事件（UP/DOWN/ADDR_ADD/ADDR_DEL）：触发 interface-only 静态路由重检查 */
            LOG_DEBUG("[route_worker] 收到 IF 事件，触发 interface-only 静态路由重检查");
            route_static_on_if_change();
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            break;

        case ROUTE_WORKER_CMD_SHUTDOWN:
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            g_route_work_local->running = 0;
            stop = 1;
            break;

        default:
            LOG_WARN("[route_worker] 未知命令类型: %d", (int)cmd->type);
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            break;
    }

    g_free(cmd);
    return stop;
}

/**
 * @brief 处理 cmd_eventfd 触发：排干所有待处理命令
 * @return 1 表示收到 SHUTDOWN，0 表示正常
 */
static int worker_drain_cmd_queue(void)
{
    uint64_t v;
    while (read(g_route_work_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
        /* 排干 eventfd 计数 */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("[route_worker] cmd eventfd read 失败");
    }

    route_worker_cmd_t *cmd = NULL;
    while ((cmd = (route_worker_cmd_t *)g_async_queue_try_pop(g_route_work_local->cmd_queue)) != NULL)
    {
        if (worker_dispatch_cmd(cmd))
        {
            return 1;
        }
    }
    return 0;
}

static void route_worker_drain_work_queue(route_work_local_t *wl)
{
    if (!wl || wl->work_eventfd < 0 || !wl->work_queue)
    {
        return;
    }

    uint64_t v;
    while (read(wl->work_eventfd, &v, sizeof(v)) > 0)
    {
        /* 排干 eventfd 计数 */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("[route_worker] work eventfd read 失败");
    }

    int processed = 0;
    route_worker_event_t *evt = NULL;
    while (processed < ROUTE_WORK_BATCH_SIZE &&
           (evt = (route_worker_event_t *)g_async_queue_try_pop(wl->work_queue)) != NULL)
    {
        switch (evt->type)
        {
            case ROUTE_WORK_EVENT_CALC:
                route_work_handle_calc_event(&evt->u.key);
                break;
            default:
                LOG_WARN("[route_worker] 未知工作事件类型: %d", (int)evt->type);
                break;
        }

        worker_event_destroy(evt);
        processed++;
    }

    if (g_async_queue_length(wl->work_queue) > 0)
    {
        worker_signal_work_event();
    }
}

// ============================================================================
// worker 线程主循环
// ============================================================================

static void *route_worker_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "route-worker");
    log_set_tag("route");

    route_work_local_t *wl = g_route_work_local;
    struct epoll_event events[ROUTE_MAX_EPOLL_EVENTS];

    LOG_INFO("[route_worker] worker 线程启动");

    /* 恢复结束后做一次全量 nexthop 重算（在 route worker 线程内执行） */
    route_recompute_iter_paths();

    while (wl->running)
    {
        int n = epoll_wait(wl->epoll_fd, events, ROUTE_MAX_EPOLL_EVENTS, 1000);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("[route_worker] epoll_wait 失败");
            break;
        }

        for (int i = 0; i < n; i++)
        {
            if (events[i].data.ptr == (void *)&g_route_cmd_tag)
            {
                if (worker_drain_cmd_queue())
                {
                    goto out;
                }
                continue;
            }

            if (events[i].data.ptr == (void *)&g_route_work_tag)
            {
                route_worker_drain_work_queue(wl);
                continue;
            }

            LOG_WARN("[route_worker] 未知 epoll 事件 ptr=%p", events[i].data.ptr);
        }

        /* 定时处理 route_calc 延迟任务（例如 OS install 失败后的重试）。 */
        route_calc_on_periodic();
    }

out:
    LOG_INFO("[route_worker] worker 线程退出");
    return NULL;
}

// ============================================================================
// 生命周期
// ============================================================================

int route_worker_prepare(void)
{
    if (!g_route_work_local)
    {
        g_route_work_local = (route_work_local_t *)g_malloc0(sizeof(route_work_local_t));
        if (!g_route_work_local)
        {
            LOG_ERROR("[route_worker] route work local 分配失败");
            return -1;
        }

        g_route_work_local->epoll_fd = -1;
        g_route_work_local->cmd_eventfd = -1;
        g_route_work_local->work_eventfd = -1;

        g_route_work_local->rib = route_rib_create();
        if (!g_route_work_local->rib)
        {
            LOG_ERROR("[route_worker] RIB 创建失败");
            goto fail;
        }

        route_static_init();
        route_calc_init();
    }

    g_route_work_local->epoll_fd = -1;
    g_route_work_local->cmd_eventfd = -1;
    g_route_work_local->work_eventfd = -1;
    g_route_work_local->running = 1;

    g_route_work_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_route_work_local->epoll_fd < 0)
    {
        LOG_PERROR("[route_worker] epoll_create1 失败");
        goto fail;
    }

    g_route_work_local->cmd_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_route_work_local->cmd_eventfd < 0)
    {
        LOG_PERROR("[route_worker] eventfd 失败");
        goto fail;
    }

    g_route_work_local->work_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_route_work_local->work_eventfd < 0)
    {
        LOG_PERROR("[route_worker] work eventfd 失败");
        goto fail;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_route_cmd_tag;
    if (epoll_ctl(g_route_work_local->epoll_fd, EPOLL_CTL_ADD, g_route_work_local->cmd_eventfd, &ev) < 0)
    {
        LOG_PERROR("[route_worker] epoll_ctl ADD cmd_eventfd 失败");
        goto fail;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_route_work_tag;
    if (epoll_ctl(g_route_work_local->epoll_fd, EPOLL_CTL_ADD, g_route_work_local->work_eventfd, &ev) < 0)
    {
        LOG_PERROR("[route_worker] epoll_ctl ADD work_eventfd 失败");
        goto fail;
    }

    g_route_work_local->cmd_queue = g_async_queue_new();
    if (!g_route_work_local->cmd_queue)
    {
        LOG_ERROR("[route_worker] 命令队列创建失败");
        goto fail;
    }

    g_route_work_local->work_queue = g_async_queue_new();
    if (!g_route_work_local->work_queue)
    {
        LOG_ERROR("[route_worker] 工作事件队列创建失败");
        goto fail;
    }

    LOG_INFO("[route_worker] worker 资源初始化完成");
    return 0;

fail:
    route_worker_shutdown();
    return -1;
}

int route_worker_launch(void)
{
    if (!g_route_work_local)
    {
        return -1;
    }

    if (pthread_create(&g_route_work_local->thread, NULL, route_worker_thread_fn, NULL) != 0)
    {
        LOG_PERROR("[route_worker] pthread_create 失败");
        return -1;
    }

    LOG_INFO("[route_worker] worker 线程已启动");
    return 0;
}

int route_worker_post(route_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    route_worker_cmd_t *cmd = worker_cmd_create(type, msg, 0);
    if (!cmd)
    {
        return -1;
    }
    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return -1;
    }
    return 0;
}

int route_worker_post_show_cli(dev_ipc_message_t *msg)
{
    route_worker_cmd_t *cmd = worker_cmd_create(ROUTE_WORKER_CMD_CLI_SHOW, msg, 0);
    if (!cmd)
    {
        return -1;
    }
    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return -1;
    }
    return 0;
}

int route_worker_dispatch_apply(route_apply_cmd_t *apply)
{
    if (!apply)
    {
        return -1;
    }

    route_worker_cmd_t *cmd = worker_cmd_create(ROUTE_WORKER_CMD_APPLY, NULL, 1);
    if (!cmd)
    {
        return -1;
    }
    cmd->apply = apply;

    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return -1;
    }

    worker_cmd_wait(cmd);
    worker_cmd_destroy(cmd);
    return 0;
}

void route_worker_shutdown(void)
{
    if (!g_route_work_local)
    {
        return;
    }

    if (g_route_work_local->running && g_route_work_local->thread != 0)
    {
        route_worker_cmd_t *cmd = (route_worker_cmd_t *)g_malloc0(sizeof(route_worker_cmd_t));
        if (cmd)
        {
            cmd->type = ROUTE_WORKER_CMD_SHUTDOWN;
            cmd->msg = NULL;
            g_async_queue_push(g_route_work_local->cmd_queue, cmd);
            worker_signal_cmd_event();
        }

        pthread_join(g_route_work_local->thread, NULL);
        g_route_work_local->thread = 0;
    }

    /* 排干剩余命令，对 waitable 命令通知失败 */
    if (g_route_work_local->cmd_queue)
    {
        route_worker_cmd_t *c = NULL;
        while ((c = (route_worker_cmd_t *)g_async_queue_try_pop(g_route_work_local->cmd_queue)) != NULL)
        {
            if (c->msg)
            {
                dev_ipc_message_free(c->msg);
            }
            if (c->waitable && !c->done)
            {
                worker_cmd_complete(c, ERRCODE_FAIL);
            }
            if (!c->waitable)
            {
                worker_cmd_destroy(c);
            }
        }
        g_async_queue_unref(g_route_work_local->cmd_queue);
        g_route_work_local->cmd_queue = NULL;
    }

    if (g_route_work_local->work_queue)
    {
        route_worker_event_t *evt = NULL;
        while ((evt = (route_worker_event_t *)g_async_queue_try_pop(g_route_work_local->work_queue)) != NULL)
        {
            worker_event_destroy(evt);
        }
        g_async_queue_unref(g_route_work_local->work_queue);
        g_route_work_local->work_queue = NULL;
    }

    if (g_route_work_local->epoll_fd >= 0)
    {
        close(g_route_work_local->epoll_fd);
        g_route_work_local->epoll_fd = -1;
    }

    if (g_route_work_local->cmd_eventfd >= 0)
    {
        close(g_route_work_local->cmd_eventfd);
        g_route_work_local->cmd_eventfd = -1;
    }

    if (g_route_work_local->work_eventfd >= 0)
    {
        close(g_route_work_local->work_eventfd);
        g_route_work_local->work_eventfd = -1;
    }

    route_relay_cleanup();
    route_show_cleanup_state();
    route_static_cleanup();
    route_calc_cleanup();

    /* 释放业务数据 */
    if (g_route_work_local->rib)
    {
        route_rib_destroy(g_route_work_local->rib);
        g_route_work_local->rib = NULL;
    }
    g_list_free_full(g_route_work_local->subscribers, g_free);
    g_route_work_local->subscribers = NULL;
    g_list_free_full(g_route_work_local->batch_entries, g_free);
    g_route_work_local->batch_entries = NULL;

    g_free(g_route_work_local);
    g_route_work_local = NULL;

    LOG_INFO("[route_worker] worker 资源已释放");
}

int route_add_and_notify(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                         uint32_t protocol, const net_addr_t *source_addr, const net_addr_t *nexthop_addr,
                         const net_addr_t *relay_addr_ptr, int32_t metric, int32_t preference, uint32_t out_ifindex)
{
    if (!g_route_work_local || !g_route_work_local->rib || !prefix_addr || !source_addr || !nexthop_addr)
    {
        return ERRCODE_FAIL;
    }

    int ret = route_rib_add(g_route_work_local->rib, vrf_id, afi, prefix_addr, prefix_len, protocol, source_addr,
                            nexthop_addr, metric, preference, out_ifindex);
    if (ret < 0)
    {
        return ret;
    }

    const route_head_t *head = route_rib_lookup_head(g_route_work_local->rib, vrf_id, afi, prefix_addr, prefix_len);
    if (!head)
    {
        return ret;
    }

    const route_path_t *path = route_rib_lookup_path(head, protocol, source_addr);
    if (!path)
    {
        return ret;
    }

    route_path_t *mut = (route_path_t *)path;
    mut->relay_addr = relay_addr_ptr ? *relay_addr_ptr : *nexthop_addr;
    mut->iter_out_ifindex = out_ifindex;

    if (route_worker_post_calc_event(&head->key) != 0)
    {
        /* 异常兜底：队列不可用时仍执行直接重算，避免路径状态卡死。 */
        route_work_handle_calc_event(&head->key);
    }

    return ret;
}
