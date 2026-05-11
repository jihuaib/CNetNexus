#include "fib_worker.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "errcode.h"
#include "fib.h"
#include "fib_main.h"
#include "fib_os.h"
#include "fib_show.h"
#include "log.h"
#include "mpls_config.h"

#define FIB_MAX_EPOLL_EVENTS 8

fib_work_local_t *g_fib_work_local = NULL;

static char g_fib_cmd_tag;

typedef struct fib_worker_cmd
{
    fib_worker_cmd_type_t type;
    dev_ipc_message_t *msg;
} fib_worker_cmd_t;

static void fib_worker_send_ack(const dev_ipc_message_t *req, int32_t result)
{
    if (!req || req->request_id == 0)
    {
        return;
    }

    fib_msg_ack_t *ack = g_malloc0(sizeof(*ack));
    if (!ack)
    {
        return;
    }
    ack->result = result;

    dev_ipc_message_t *resp = dev_ipc_message_create(FIB_MSG_TYPE_ACK, DEV_MODULE_ID_FIB, req->src_module_id,
                                                     req->request_id, ack, sizeof(*ack), g_free);
    if (!resp)
    {
        g_free(ack);
        return;
    }
    dev_ipc_send_response(fib_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
}

static void fib_worker_send_route_result(const dev_ipc_message_t *req, const fib_route_entry_t *entry, int32_t result,
                                         uint32_t op_msg_type)
{
    if (!req || !entry || req->src_module_id != DEV_MODULE_ID_ROUTE)
    {
        return;
    }

    fib_route_result_t *payload = g_malloc0(sizeof(*payload));
    if (!payload)
    {
        return;
    }
    payload->entry = *entry;
    payload->result = result;
    payload->op_msg_type = op_msg_type;

    dev_ipc_message_t *msg = dev_ipc_message_create(FIB_MSG_TYPE_ROUTE_RESULT, DEV_MODULE_ID_FIB, req->src_module_id, 0,
                                                    payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return;
    }
    (void)dev_ipc_send(fib_local_ipc_ctx(), req->src_module_id, msg);
    dev_ipc_message_free(msg);
}

static fib_worker_cmd_t *fib_worker_cmd_create(fib_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    fib_worker_cmd_t *cmd = g_malloc0(sizeof(*cmd));
    if (!cmd)
    {
        return NULL;
    }
    cmd->type = type;
    cmd->msg = msg;
    return cmd;
}

static void fib_worker_cmd_destroy(fib_worker_cmd_t *cmd)
{
    if (!cmd)
    {
        return;
    }
    if (cmd->msg)
    {
        dev_ipc_message_free(cmd->msg);
    }
    g_free(cmd);
}

static void fib_signal_cmd_event(void)
{
    if (!g_fib_work_local || g_fib_work_local->cmd_eventfd < 0)
    {
        return;
    }

    uint64_t one = 1;
    if (write(g_fib_work_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("FIB: cmd eventfd write failed");
        }
    }
}

static int fib_worker_cmd_enqueue(fib_worker_cmd_t *cmd)
{
    if (!cmd || !g_fib_work_local || !g_fib_work_local->cmd_queue || g_fib_work_local->cmd_eventfd < 0)
    {
        return ERRCODE_FAIL;
    }

    g_async_queue_push(g_fib_work_local->cmd_queue, cmd);
    fib_signal_cmd_event();
    return ERRCODE_SUCCESS;
}

static int fib_worker_prepare_os(void)
{
    nn_mpls_config_t cfg;
    if (nn_mpls_config_load(&cfg) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("FIB: failed to load MPLS config for kernel programming");
        return ERRCODE_FAIL;
    }

    return fib_os_mpls_configure(cfg.linux_platform_labels, cfg.linux_mpls_input);
}

static int tunnel_ready(const fib_tunnel_entry_t *tunnel)
{
    return tunnel && tunnel->state && tunnel->label_count > 0 &&
           (tunnel->relay_addr.family == AF_INET || tunnel->relay_addr.family == AF_INET6);
}

static int fib_reconcile_route(fib_route_state_t *route_state)
{
    if (!route_state || !g_fib_work_local || !g_fib_work_local->rib)
    {
        return ERRCODE_FAIL;
    }

    const fib_route_entry_t *route = &route_state->entry;
    if ((route->flags & FIB_ROUTE_FLAG_SKIP_OS) != 0)
    {
        route_state->installed = 0u;
        return ERRCODE_SUCCESS;
    }

    int rc = ERRCODE_FAIL;
    if (route->nh_type == FIB_NH_TYPE_IP || route->nh_type == FIB_NH_TYPE_BLACKHOLE)
    {
        rc = fib_os_route_install_ip(route);
    }
    else if (route->nh_type == FIB_NH_TYPE_TUNNEL)
    {
        fib_tunnel_state_t *tun = fib_rib_tunnel_lookup(g_fib_work_local->rib, route->tunnel_id);
        if (!tun || !tunnel_ready(&tun->entry))
        {
            if (route_state->installed)
            {
                (void)fib_os_route_withdraw(route);
                route_state->installed = 0u;
            }
            LOG_DEBUG("FIB: route pending tunnel=%u", route->tunnel_id);
            return ERRCODE_SUCCESS;
        }
        rc = fib_os_route_install_tunnel(route, &tun->entry);
    }

    route_state->installed = (rc == ERRCODE_SUCCESS) ? 1u : 0u;
    return rc;
}

typedef struct fib_reconcile_tunnel_ctx
{
    uint32_t tunnel_id;
} fib_reconcile_tunnel_ctx_t;

static void fib_reconcile_tunnel_route_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    fib_route_state_t *route = (fib_route_state_t *)value;
    const fib_reconcile_tunnel_ctx_t *ctx = (const fib_reconcile_tunnel_ctx_t *)user_data;
    if (!route || !ctx || route->entry.nh_type != FIB_NH_TYPE_TUNNEL || route->entry.tunnel_id != ctx->tunnel_id)
    {
        return;
    }
    (void)fib_reconcile_route(route);
}

static void fib_reconcile_tunnel(uint32_t tunnel_id)
{
    fib_reconcile_tunnel_ctx_t ctx = {.tunnel_id = tunnel_id};
    fib_rib_foreach_route(g_fib_work_local ? g_fib_work_local->rib : NULL, fib_reconcile_tunnel_route_cb, &ctx);
}

static int fib_handle_route_upsert(fib_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->msg || !cmd->msg->payload || cmd->msg->payload_len < sizeof(fib_route_entry_t))
    {
        return ERRCODE_FAIL;
    }

    const fib_route_entry_t *entry = (const fib_route_entry_t *)cmd->msg->payload;
    fib_route_entry_t old_entry;
    uint8_t old_installed = 0u;
    fib_route_state_t *old_state = fib_rib_route_lookup(g_fib_work_local->rib, entry);
    if (old_state)
    {
        old_entry = old_state->entry;
        old_installed = old_state->installed;
    }

    fib_route_state_t *state = fib_rib_route_upsert(g_fib_work_local->rib, entry);
    if (state && (entry->flags & FIB_ROUTE_FLAG_SKIP_OS) != 0)
    {
        if (old_state && old_installed)
        {
            (void)fib_os_route_withdraw(&old_entry);
        }
        state->installed = 0u;
        fib_worker_send_ack(cmd->msg, ERRCODE_SUCCESS);
        return ERRCODE_SUCCESS;
    }

    int rc = state ? fib_reconcile_route(state) : ERRCODE_FAIL;
    if (rc != ERRCODE_SUCCESS)
    {
        fib_worker_send_route_result(cmd->msg, entry, rc, FIB_MSG_TYPE_ROUTE_UPSERT);
    }
    fib_worker_send_ack(cmd->msg, rc);
    return rc;
}

static int fib_handle_route_delete(fib_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->msg || !cmd->msg->payload || cmd->msg->payload_len < sizeof(fib_route_entry_t))
    {
        return ERRCODE_FAIL;
    }

    fib_route_entry_t old_entry;
    uint8_t installed = 0u;
    const fib_route_entry_t *entry = (const fib_route_entry_t *)cmd->msg->payload;
    gboolean removed = fib_rib_route_delete(g_fib_work_local->rib, entry, &old_entry, &installed);
    if (removed && installed)
    {
        (void)fib_os_route_withdraw(&old_entry);
    }
    fib_worker_send_ack(cmd->msg, ERRCODE_SUCCESS);
    return ERRCODE_SUCCESS;
}

static int fib_handle_tunnel_upsert(fib_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->msg || !cmd->msg->payload || cmd->msg->payload_len < sizeof(fib_tunnel_entry_t))
    {
        return ERRCODE_FAIL;
    }

    const fib_tunnel_entry_t *entry = (const fib_tunnel_entry_t *)cmd->msg->payload;
    fib_tunnel_state_t *state = fib_rib_tunnel_upsert(g_fib_work_local->rib, entry);
    int rc = state ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    if (rc == ERRCODE_SUCCESS)
    {
        fib_reconcile_tunnel(entry->tunnel_id);
    }
    fib_worker_send_ack(cmd->msg, rc);
    return rc;
}

static int fib_handle_tunnel_delete(fib_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->msg || !cmd->msg->payload || cmd->msg->payload_len < sizeof(fib_tunnel_entry_t))
    {
        return ERRCODE_FAIL;
    }

    const fib_tunnel_entry_t *entry = (const fib_tunnel_entry_t *)cmd->msg->payload;
    (void)fib_rib_tunnel_delete(g_fib_work_local->rib, entry->tunnel_id);
    fib_reconcile_tunnel(entry->tunnel_id);
    fib_worker_send_ack(cmd->msg, ERRCODE_SUCCESS);
    return ERRCODE_SUCCESS;
}

static int fib_handle_ilm_upsert(fib_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->msg || !cmd->msg->payload || cmd->msg->payload_len < sizeof(fib_ilm_entry_t))
    {
        return ERRCODE_FAIL;
    }

    const fib_ilm_entry_t *entry = (const fib_ilm_entry_t *)cmd->msg->payload;
    fib_ilm_entry_t old_entry;
    uint8_t old_installed = 0u;
    fib_ilm_state_t *old_state = fib_rib_ilm_lookup(g_fib_work_local->rib, entry->vrf_id, entry->in_label);
    if (old_state)
    {
        old_entry = old_state->entry;
        old_installed = old_state->installed;
    }

    fib_ilm_state_t *state = fib_rib_ilm_upsert(g_fib_work_local->rib, entry);
    int rc = state ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    if (rc == ERRCODE_SUCCESS && old_state && old_installed)
    {
        (void)fib_os_ilm_withdraw(&old_entry);
        state->installed = 0u;
    }
    if (rc == ERRCODE_SUCCESS && entry->state)
    {
        rc = fib_os_ilm_install(entry);
        state->installed = (rc == ERRCODE_SUCCESS) ? 1u : 0u;
    }
    fib_worker_send_ack(cmd->msg, rc);
    return rc;
}

static int fib_handle_ilm_delete(fib_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->msg || !cmd->msg->payload || cmd->msg->payload_len < sizeof(fib_ilm_entry_t))
    {
        return ERRCODE_FAIL;
    }

    fib_ilm_entry_t old_entry;
    uint8_t installed = 0u;
    const fib_ilm_entry_t *entry = (const fib_ilm_entry_t *)cmd->msg->payload;
    gboolean removed = fib_rib_ilm_delete(g_fib_work_local->rib, entry, &old_entry, &installed);
    if (removed && installed)
    {
        (void)fib_os_ilm_withdraw(&old_entry);
    }
    fib_worker_send_ack(cmd->msg, ERRCODE_SUCCESS);
    return ERRCODE_SUCCESS;
}

static int fib_worker_dispatch_cmd(fib_worker_cmd_t *cmd)
{
    int stop = 0;

    switch (cmd->type)
    {
        case FIB_WORKER_CMD_ROUTE_UPSERT:
            (void)fib_handle_route_upsert(cmd);
            break;
        case FIB_WORKER_CMD_ROUTE_DELETE:
            (void)fib_handle_route_delete(cmd);
            break;
        case FIB_WORKER_CMD_TUNNEL_UPSERT:
            (void)fib_handle_tunnel_upsert(cmd);
            break;
        case FIB_WORKER_CMD_TUNNEL_DELETE:
            (void)fib_handle_tunnel_delete(cmd);
            break;
        case FIB_WORKER_CMD_ILM_UPSERT:
            (void)fib_handle_ilm_upsert(cmd);
            break;
        case FIB_WORKER_CMD_ILM_DELETE:
            (void)fib_handle_ilm_delete(cmd);
            break;
        case FIB_WORKER_CMD_SHOW_CLI:
            (void)fib_show_dispatch(cmd->msg);
            break;
        case FIB_WORKER_CMD_SHUTDOWN:
            g_fib_work_local->running = 0;
            stop = 1;
            break;
        default:
            LOG_WARN("FIB: unknown worker command %d", (int)cmd->type);
            break;
    }

    fib_worker_cmd_destroy(cmd);
    return stop;
}

static int fib_worker_drain_cmd_queue(void)
{
    uint64_t v;
    while (read(g_fib_work_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("FIB: cmd eventfd read failed");
    }

    fib_worker_cmd_t *cmd = NULL;
    while ((cmd = g_async_queue_try_pop(g_fib_work_local->cmd_queue)) != NULL)
    {
        if (fib_worker_dispatch_cmd(cmd))
        {
            return 1;
        }
    }
    return 0;
}

static void *fib_worker_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "fib-worker");
    log_set_tag("fib");

    struct epoll_event events[FIB_MAX_EPOLL_EVENTS];
    LOG_INFO("FIB: worker thread started");
    while (g_fib_work_local->running)
    {
        int n = epoll_wait(g_fib_work_local->epoll_fd, events, FIB_MAX_EPOLL_EVENTS, 1000);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("FIB: epoll_wait failed");
            break;
        }

        for (int i = 0; i < n; ++i)
        {
            if (events[i].data.ptr == &g_fib_cmd_tag && fib_worker_drain_cmd_queue())
            {
                break;
            }
        }
    }

    LOG_INFO("FIB: worker thread stopped");
    return NULL;
}

int fib_worker_prepare(void)
{
    if (g_fib_work_local)
    {
        return ERRCODE_SUCCESS;
    }

    g_fib_work_local = g_malloc0(sizeof(*g_fib_work_local));
    if (!g_fib_work_local)
    {
        return ERRCODE_FAIL;
    }

    g_fib_work_local->epoll_fd = -1;
    g_fib_work_local->cmd_eventfd = -1;
    g_fib_work_local->rib = fib_rib_create();
    g_fib_work_local->cmd_queue = g_async_queue_new();
    g_fib_work_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    g_fib_work_local->cmd_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (!g_fib_work_local->rib || !g_fib_work_local->cmd_queue || g_fib_work_local->epoll_fd < 0 ||
        g_fib_work_local->cmd_eventfd < 0)
    {
        fib_worker_shutdown();
        return ERRCODE_FAIL;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_fib_cmd_tag;
    if (epoll_ctl(g_fib_work_local->epoll_fd, EPOLL_CTL_ADD, g_fib_work_local->cmd_eventfd, &ev) != 0)
    {
        fib_worker_shutdown();
        return ERRCODE_FAIL;
    }
    if (fib_worker_prepare_os() != ERRCODE_SUCCESS)
    {
        fib_worker_shutdown();
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int fib_worker_launch(void)
{
    if (!g_fib_work_local)
    {
        return ERRCODE_FAIL;
    }
    if (g_fib_work_local->running)
    {
        return ERRCODE_SUCCESS;
    }

    g_fib_work_local->running = 1;
    if (pthread_create(&g_fib_work_local->thread, NULL, fib_worker_thread_fn, NULL) != 0)
    {
        g_fib_work_local->running = 0;
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int fib_worker_post(fib_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    fib_worker_cmd_t *cmd = fib_worker_cmd_create(type, msg);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (fib_worker_cmd_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        fib_worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

void fib_worker_shutdown(void)
{
    if (!g_fib_work_local)
    {
        return;
    }

    if (g_fib_work_local->running)
    {
        (void)fib_worker_post(FIB_WORKER_CMD_SHUTDOWN, NULL);
        pthread_join(g_fib_work_local->thread, NULL);
        g_fib_work_local->running = 0;
    }

    if (g_fib_work_local->cmd_queue)
    {
        fib_worker_cmd_t *cmd = NULL;
        while ((cmd = g_async_queue_try_pop(g_fib_work_local->cmd_queue)) != NULL)
        {
            fib_worker_cmd_destroy(cmd);
        }
        g_async_queue_unref(g_fib_work_local->cmd_queue);
    }
    if (g_fib_work_local->cmd_eventfd >= 0)
    {
        close(g_fib_work_local->cmd_eventfd);
    }
    if (g_fib_work_local->epoll_fd >= 0)
    {
        close(g_fib_work_local->epoll_fd);
    }
    fib_rib_destroy(g_fib_work_local->rib);
    fib_show_cleanup();
    g_free(g_fib_work_local);
    g_fib_work_local = NULL;
}
