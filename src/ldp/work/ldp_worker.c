/**
 * @file   ldp_worker.c
 * @brief  LDP worker 线程：epoll + 1Hz tick + 命令队列 + 接口/邻接管理
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "errcode.h"
#include "if.h"
#include "ldp.h"
#include "ldp_discovery.h"
#include "ldp_lib.h"
#include "ldp_main.h"
#include "ldp_route_sync.h"
#include "ldp_session.h"
#include "ldp_show.h"
#include "log.h"

ldp_work_local_t *g_ldp_work_local = NULL;

typedef enum ldp_worker_cmd_type
{
    LDP_WORKER_CMD_SHOW = 1,
    LDP_WORKER_CMD_IF_EVENT = 2,
    LDP_WORKER_CMD_APPLY = 3,
    LDP_WORKER_CMD_ROUTE_MSG = 4,
    LDP_WORKER_CMD_SHUTDOWN = 5,
    LDP_WORKER_CMD_ROUTE_READY = 6,
    LDP_WORKER_CMD_IF_DOWN = 7,
} ldp_worker_cmd_type_t;

typedef struct ldp_worker_cmd
{
    ldp_worker_cmd_type_t type;
    dev_ipc_message_t *msg;
    ldp_apply_cmd_t *apply;

    int waitable;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int rc;
} ldp_worker_cmd_t;

// ============================================================================
// 工具
// ============================================================================

uint64_t ldp_worker_now_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

uint32_t ldp_worker_effective_hello_ms(const ldp_iface_state_t *iface)
{
    if (iface && iface->hello_interval_ms != 0u)
    {
        return iface->hello_interval_ms;
    }
    if (g_ldp_work_local && g_ldp_work_local->proto.hello_interval_ms != 0u)
    {
        return g_ldp_work_local->proto.hello_interval_ms;
    }
    return LDP_DEFAULT_HELLO_INTERVAL_MS;
}

uint32_t ldp_worker_effective_hold_ms(const ldp_iface_state_t *iface)
{
    if (iface && iface->hold_time_ms != 0u)
    {
        return iface->hold_time_ms;
    }
    if (g_ldp_work_local && g_ldp_work_local->proto.hold_time_ms != 0u)
    {
        return g_ldp_work_local->proto.hold_time_ms;
    }
    return LDP_DEFAULT_HOLD_TIME_MS;
}

void ldp_worker_format_lsr_id(uint32_t lsr_id_host, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0)
    {
        return;
    }
    snprintf(buf, buf_len, "%u.%u.%u.%u", (lsr_id_host >> 24) & 0xFFu, (lsr_id_host >> 16) & 0xFFu,
             (lsr_id_host >> 8) & 0xFFu, lsr_id_host & 0xFFu);
}

guint64 ldp_worker_adj_key(uint32_t peer_lsr_id, uint16_t label_space)
{
    return ((guint64)peer_lsr_id << 16) | (guint64)label_space;
}

// ============================================================================
// 命令队列
// ============================================================================

static ldp_worker_cmd_t *worker_cmd_create(ldp_worker_cmd_type_t type, dev_ipc_message_t *msg, int waitable)
{
    ldp_worker_cmd_t *cmd = g_malloc0(sizeof(*cmd));
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

static void worker_cmd_destroy(ldp_worker_cmd_t *cmd)
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

static void worker_cmd_complete(ldp_worker_cmd_t *cmd, int rc)
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

static int worker_cmd_wait(ldp_worker_cmd_t *cmd)
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
    if (!g_ldp_work_local || g_ldp_work_local->cmd_eventfd < 0)
    {
        return;
    }
    uint64_t one = 1;
    if (write(g_ldp_work_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("LDP: write cmd_eventfd");
        }
    }
}

static int worker_cmd_enqueue(ldp_worker_cmd_t *cmd)
{
    if (!cmd || !g_ldp_work_local || !g_ldp_work_local->cmd_queue)
    {
        return -1;
    }
    g_async_queue_push(g_ldp_work_local->cmd_queue, cmd);
    worker_signal_cmd_event();
    return 0;
}

// ============================================================================
// apply 处理
// ============================================================================

static int worker_apply_cmd(ldp_apply_cmd_t *apply)
{
    if (!apply || !g_ldp_work_local)
    {
        return ERRCODE_FAIL;
    }

    switch (apply->op)
    {
        case LDP_APPLY_OP_PROTO_SET:
        {
            uint8_t admin_changed = (g_ldp_work_local->proto.admin_up != apply->u.proto.admin_up) ? 1u : 0u;
            uint8_t lsr_changed = (g_ldp_work_local->proto.lsr_id != apply->u.proto.lsr_id) ? 1u : 0u;
            g_ldp_work_local->proto = apply->u.proto;
            if (g_ldp_work_local->proto.admin_up && g_ldp_work_local->proto.lsr_id != 0u)
            {
                ldp_route_sync_subscribe();
            }
            else
            {
                ldp_route_sync_unsubscribe();
            }
            ldp_discovery_proto_changed(admin_changed, lsr_changed);
            return ERRCODE_SUCCESS;
        }
        case LDP_APPLY_OP_IF_SET:
            return ldp_discovery_iface_set(&apply->u.if_set);
        case LDP_APPLY_OP_IF_DEL:
            return ldp_discovery_iface_del(apply->u.if_del.ifname);
        default:
            return ERRCODE_FAIL;
    }
}

// ============================================================================
// 命令分发
// ============================================================================

static const char *if_event_get_logical_name(const dev_ipc_message_t *msg, char *out_buf, size_t out_sz)
{
    if (!msg || !msg->payload || !out_buf || out_sz == 0)
    {
        return NULL;
    }
    out_buf[0] = '\0';

    if (msg->payload_len >= sizeof(if_addr_event_msg_t))
    {
        const if_addr_event_msg_t *addr_evt = (const if_addr_event_msg_t *)msg->payload;
        if (addr_evt->event == IF_EVENT_PROTO_UP || addr_evt->event == IF_EVENT_PROTO_DOWN)
        {
            g_strlcpy(out_buf, addr_evt->logical_name, out_sz);
            return out_buf[0] ? out_buf : NULL;
        }
    }
    if (msg->payload_len >= sizeof(if_event_msg_t))
    {
        const if_event_msg_t *evt = (const if_event_msg_t *)msg->payload;
        g_strlcpy(out_buf, evt->logical_name, out_sz);
        return out_buf[0] ? out_buf : NULL;
    }
    return NULL;
}

static int worker_dispatch_cmd(ldp_worker_cmd_t *cmd)
{
    if (!cmd)
    {
        return 0;
    }

    switch (cmd->type)
    {
        case LDP_WORKER_CMD_SHOW:
            (void)ldp_show_handle_msg(cmd->msg);
            cmd->msg = NULL;
            break;

        case LDP_WORKER_CMD_IF_EVENT:
        {
            char ifname[IF_LOGICAL_NAME_MAX] = {0};
            const char *name = if_event_get_logical_name(cmd->msg, ifname, sizeof(ifname));
            if_api_cache_on_event(cmd->msg);
            if (name && name[0])
            {
                ldp_discovery_on_if_event(name);
            }
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            break;
        }

        case LDP_WORKER_CMD_APPLY:
            if (cmd->apply)
            {
                cmd->apply->rc = worker_apply_cmd(cmd->apply);
            }
            worker_cmd_complete(cmd, ERRCODE_SUCCESS);
            return 0;

        case LDP_WORKER_CMD_ROUTE_MSG:
            ldp_route_sync_on_route_msg(cmd->msg);
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            break;

        case LDP_WORKER_CMD_ROUTE_READY:
            if (dev_ipc_wait_connected(ldp_local_ipc_ctx(), DEV_MODULE_ID_ROUTE, DEV_IPC_WAIT_PEER_MS) !=
                ERRCODE_SUCCESS)
            {
                LOG_WARN("LDP: ROUTE connection not ready in time; resubscribe deferred to next READY");
                break;
            }
            if (g_ldp_work_local->proto.admin_up && g_ldp_work_local->proto.lsr_id != 0u)
            {
                ldp_route_sync_resubscribe();
            }
            break;

        case LDP_WORKER_CMD_IF_DOWN:
        {
            /* IF 模块下线：
             *   1) 清掉 IF 共享缓存。
             *   2) 对每个本地已知接口调 ldp_discovery_on_if_event：因为缓存被清空，
             *      refresh_iface_from_cache 会判 !link_up，触发 close_iface_socket +
             *      purge_adjacencies_for_iface → 级联 ldp_session_on_adjacency_down
             *      关闭 TCP 会话。
             * IF READY 后由 if_api_subscribe_all 重新拉接口状态，本地接口会再次激活。 */
            LOG_INFO("LDP: IF DOWN detected, flushing IF cache + tearing down adjacencies/sessions");
            if_api_cache_cleanup();
            if_api_cache_init();
            if (g_ldp_work_local && g_ldp_work_local->interfaces)
            {
                /* 复制 key 列表，避免回调内部修改 interfaces 时迭代器失效 */
                GList *names = g_hash_table_get_keys(g_ldp_work_local->interfaces);
                for (GList *it = names; it; it = it->next)
                {
                    const char *ifname = (const char *)it->data;
                    if (ifname && ifname[0])
                    {
                        ldp_discovery_on_if_event(ifname);
                    }
                }
                g_list_free(names);
            }
            break;
        }

        case LDP_WORKER_CMD_SHUTDOWN:
            g_ldp_work_local->running = 0;
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
    while (read(g_ldp_work_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
        /* drain */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("LDP: cmd eventfd read");
    }

    ldp_worker_cmd_t *cmd = NULL;
    while ((cmd = (ldp_worker_cmd_t *)g_async_queue_try_pop(g_ldp_work_local->cmd_queue)) != NULL)
    {
        if (worker_dispatch_cmd(cmd))
        {
            return 1;
        }
    }
    return 0;
}

// ============================================================================
// epoll 主循环
// ============================================================================

static ldp_iface_state_t *iface_lookup_by_fd(int fd)
{
    if (!g_ldp_work_local || !g_ldp_work_local->interfaces || fd < 0)
    {
        return NULL;
    }
    GHashTableIter it;
    gpointer k = NULL, v = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->interfaces);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        ldp_iface_state_t *iface = (ldp_iface_state_t *)v;
        if (iface && iface->udp_fd == fd)
        {
            return iface;
        }
    }
    return NULL;
}

static ldp_peer_t *peer_lookup_by_fd(int fd)
{
    if (!g_ldp_work_local || !g_ldp_work_local->peers || fd < 0)
    {
        return NULL;
    }
    GHashTableIter it;
    gpointer k = NULL, v = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->peers);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        ldp_peer_t *p = (ldp_peer_t *)v;
        if (p && p->fd == fd)
        {
            return p;
        }
    }
    return NULL;
}

static void *ldp_worker_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "ldp-worker");
    log_set_tag("ldp");

    struct epoll_event events[LDP_MAX_EPOLL_EVENTS];
    while (g_ldp_work_local->running)
    {
        int n = epoll_wait(g_ldp_work_local->epoll_fd, events, LDP_MAX_EPOLL_EVENTS, 1000);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("LDP: epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++)
        {
            uint64_t tag = events[i].data.u64;
            ldp_evt_kind_t kind = LDP_EVT_KIND(tag);
            int fd = LDP_EVT_FD(tag);
            uint32_t ev = events[i].events;

            switch (kind)
            {
                case LDP_EVT_KIND_CMD:
                    if (worker_drain_cmd_queue())
                    {
                        return NULL;
                    }
                    break;
                case LDP_EVT_KIND_TICK:
                {
                    uint64_t v;
                    while (read(g_ldp_work_local->tick_fd, &v, sizeof(v)) > 0)
                    {
                    }
                    ldp_discovery_tick();
                    ldp_session_tick();
                    break;
                }
                case LDP_EVT_KIND_LISTEN:
                    ldp_session_handle_listen_accept();
                    break;
                case LDP_EVT_KIND_IFACE:
                {
                    /* 按 fd 反查；查不到说明 iface 已被关闭/释放，安全跳过 */
                    ldp_iface_state_t *iface = iface_lookup_by_fd(fd);
                    if (iface)
                    {
                        ldp_discovery_handle_rx(iface);
                    }
                    break;
                }
                case LDP_EVT_KIND_PEER:
                {
                    /* 按 fd 反查；同批事件中如果 peer 已被 tick/apply 释放，
                     * 反查返回 NULL，不会触发 use-after-free */
                    ldp_peer_t *peer = peer_lookup_by_fd(fd);
                    if (peer)
                    {
                        ldp_session_handle_io(peer, ev);
                    }
                    break;
                }
                case LDP_EVT_KIND_PENDING_ACCEPT:
                    ldp_session_pending_handle_io(fd, ev);
                    break;
                default:
                    break;
            }
        }
    }
    return NULL;
}

// ============================================================================
// 销毁辅助
// ============================================================================

static void ldp_iface_state_free(gpointer data)
{
    ldp_iface_state_t *iface = (ldp_iface_state_t *)data;
    if (!iface)
    {
        return;
    }
    if (iface->udp_fd >= 0)
    {
        if (g_ldp_work_local && g_ldp_work_local->epoll_fd >= 0)
        {
            (void)epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_DEL, iface->udp_fd, NULL);
        }
        close(iface->udp_fd);
        iface->udp_fd = -1;
    }
    g_free(iface);
}

static void ldp_adjacency_free(gpointer data)
{
    g_free(data);
}

// ============================================================================
// 生命周期
// ============================================================================

int ldp_worker_prepare(void)
{
    if (!g_ldp_work_local)
    {
        g_ldp_work_local = g_malloc0(sizeof(*g_ldp_work_local));
        if (!g_ldp_work_local)
        {
            return ERRCODE_FAIL;
        }
    }

    g_ldp_work_local->proto.hello_interval_ms = LDP_DEFAULT_HELLO_INTERVAL_MS;
    g_ldp_work_local->proto.hold_time_ms = LDP_DEFAULT_HOLD_TIME_MS;
    g_ldp_work_local->proto.keepalive_ms = LDP_DEFAULT_KEEPALIVE_INTERVAL_MS;

    g_ldp_work_local->interfaces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ldp_iface_state_free);
    g_ldp_work_local->adjacencies = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, ldp_adjacency_free);
    g_ldp_work_local->peers = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    g_ldp_work_local->tcp_listen_fd = -1;
    if (!g_ldp_work_local->interfaces || !g_ldp_work_local->adjacencies || !g_ldp_work_local->peers)
    {
        return ERRCODE_FAIL;
    }

    if_api_cache_init();
    ldp_lib_init();
    ldp_route_sync_init();

    g_ldp_work_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    g_ldp_work_local->cmd_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    g_ldp_work_local->tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_ldp_work_local->epoll_fd < 0 || g_ldp_work_local->cmd_eventfd < 0 || g_ldp_work_local->tick_fd < 0)
    {
        return ERRCODE_FAIL;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 1;
    its.it_interval.tv_sec = 1;
    if (timerfd_settime(g_ldp_work_local->tick_fd, 0, &its, NULL) < 0)
    {
        LOG_PERROR("LDP: timerfd_settime");
        return ERRCODE_FAIL;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = LDP_EVT_PACK(LDP_EVT_KIND_CMD, g_ldp_work_local->cmd_eventfd);
    if (epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_ADD, g_ldp_work_local->cmd_eventfd, &ev) < 0)
    {
        return ERRCODE_FAIL;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = LDP_EVT_PACK(LDP_EVT_KIND_TICK, g_ldp_work_local->tick_fd);
    if (epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_ADD, g_ldp_work_local->tick_fd, &ev) < 0)
    {
        return ERRCODE_FAIL;
    }

    g_ldp_work_local->cmd_queue = g_async_queue_new();
    if (!g_ldp_work_local->cmd_queue)
    {
        return ERRCODE_FAIL;
    }

    if (ldp_session_open_listener() < 0)
    {
        LOG_WARN("LDP: TCP listener not available; passive sessions disabled");
    }

    g_ldp_work_local->running = 1;
    return ERRCODE_SUCCESS;
}

int ldp_worker_launch(void)
{
    if (!g_ldp_work_local)
    {
        return ERRCODE_FAIL;
    }
    if (pthread_create(&g_ldp_work_local->thread, NULL, ldp_worker_thread_fn, NULL) != 0)
    {
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

void ldp_worker_shutdown(void)
{
    if (!g_ldp_work_local)
    {
        return;
    }

    if (g_ldp_work_local->running && g_ldp_work_local->thread != 0)
    {
        ldp_worker_cmd_t *cmd = g_malloc0(sizeof(*cmd));
        if (cmd)
        {
            cmd->type = LDP_WORKER_CMD_SHUTDOWN;
            g_async_queue_push(g_ldp_work_local->cmd_queue, cmd);
            worker_signal_cmd_event();
        }
        pthread_join(g_ldp_work_local->thread, NULL);
        g_ldp_work_local->thread = 0;
    }

    if (g_ldp_work_local->cmd_queue)
    {
        ldp_worker_cmd_t *cmd = NULL;
        while ((cmd = (ldp_worker_cmd_t *)g_async_queue_try_pop(g_ldp_work_local->cmd_queue)) != NULL)
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
        g_async_queue_unref(g_ldp_work_local->cmd_queue);
        g_ldp_work_local->cmd_queue = NULL;
    }

    ldp_session_close_all();
    ldp_session_close_listener();

    if (g_ldp_work_local->cmd_eventfd >= 0)
    {
        close(g_ldp_work_local->cmd_eventfd);
        g_ldp_work_local->cmd_eventfd = -1;
    }
    if (g_ldp_work_local->tick_fd >= 0)
    {
        close(g_ldp_work_local->tick_fd);
        g_ldp_work_local->tick_fd = -1;
    }
    if (g_ldp_work_local->epoll_fd >= 0)
    {
        close(g_ldp_work_local->epoll_fd);
        g_ldp_work_local->epoll_fd = -1;
    }

    ldp_show_cleanup();
    ldp_route_sync_cleanup();
    ldp_lib_cleanup();

    if (g_ldp_work_local->interfaces)
    {
        g_hash_table_destroy(g_ldp_work_local->interfaces);
        g_ldp_work_local->interfaces = NULL;
    }
    if (g_ldp_work_local->adjacencies)
    {
        g_hash_table_destroy(g_ldp_work_local->adjacencies);
        g_ldp_work_local->adjacencies = NULL;
    }
    if (g_ldp_work_local->peers)
    {
        g_hash_table_destroy(g_ldp_work_local->peers);
        g_ldp_work_local->peers = NULL;
    }

    g_free(g_ldp_work_local);
    g_ldp_work_local = NULL;
}

// ============================================================================
// 外部派发入口
// ============================================================================

int ldp_worker_post_show_cli(dev_ipc_message_t *msg)
{
    ldp_worker_cmd_t *cmd = worker_cmd_create(LDP_WORKER_CMD_SHOW, msg, 0);
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

int ldp_worker_post_if_event(dev_ipc_message_t *msg)
{
    ldp_worker_cmd_t *cmd = worker_cmd_create(LDP_WORKER_CMD_IF_EVENT, msg, 0);
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

int ldp_worker_post_route_msg(dev_ipc_message_t *msg)
{
    ldp_worker_cmd_t *cmd = worker_cmd_create(LDP_WORKER_CMD_ROUTE_MSG, msg, 0);
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

int ldp_worker_post_if_down(void)
{
    ldp_worker_cmd_t *cmd = worker_cmd_create(LDP_WORKER_CMD_IF_DOWN, NULL, 0);
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

int ldp_worker_post_route_ready(void)
{
    ldp_worker_cmd_t *cmd = worker_cmd_create(LDP_WORKER_CMD_ROUTE_READY, NULL, 0);
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

int ldp_worker_dispatch_apply(ldp_apply_cmd_t *apply)
{
    if (!apply)
    {
        return ERRCODE_FAIL;
    }
    ldp_worker_cmd_t *cmd = worker_cmd_create(LDP_WORKER_CMD_APPLY, NULL, 1);
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
