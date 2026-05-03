#include "tunnel_worker.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cli.h"
#include "errcode.h"
#include "log.h"
#include "tunnel.h"
#include "tunnel_main.h"
#include "tunnel_show.h"

#define TUNNEL_MAX_EPOLL_EVENTS 8

tunnel_work_local_t *g_tunnel_work_local = NULL;

static char g_tunnel_cmd_tag;

typedef struct tunnel_worker_cmd
{
    tunnel_worker_cmd_type_t type;
    dev_ipc_message_t *msg;
} tunnel_worker_cmd_t;

static void worker_send_ack_label(const dev_ipc_message_t *req, int32_t result, uint32_t label)
{
    if (!req || req->request_id == 0)
    {
        return;
    }

    tunnel_msg_ack_t *ack = g_malloc0(sizeof(*ack));
    if (!ack)
    {
        return;
    }
    ack->result = result;
    ack->label = label;

    dev_ipc_message_t *resp = dev_ipc_message_create(TUNNEL_MSG_TYPE_ACK, DEV_MODULE_ID_TUNNEL, req->src_module_id,
                                                     req->request_id, ack, sizeof(*ack), g_free);
    if (!resp)
    {
        g_free(ack);
        return;
    }
    dev_ipc_send_response(tunnel_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
}

static void worker_send_ack(const dev_ipc_message_t *req, int32_t result)
{
    worker_send_ack_label(req, result, 0u);
}

static tunnel_worker_cmd_t *worker_cmd_create(tunnel_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    tunnel_worker_cmd_t *cmd = g_malloc0(sizeof(*cmd));
    if (!cmd)
    {
        return NULL;
    }
    cmd->type = type;
    cmd->msg = msg;
    return cmd;
}

static void worker_cmd_destroy(tunnel_worker_cmd_t *cmd)
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

static void worker_signal_cmd_event(void)
{
    if (!g_tunnel_work_local || g_tunnel_work_local->cmd_eventfd < 0)
    {
        return;
    }

    uint64_t one = 1;
    if (write(g_tunnel_work_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("TUNNEL: cmd eventfd write failed");
        }
    }
}

static int worker_cmd_enqueue(tunnel_worker_cmd_t *cmd)
{
    if (!cmd || !g_tunnel_work_local || !g_tunnel_work_local->cmd_queue || g_tunnel_work_local->cmd_eventfd < 0)
    {
        return ERRCODE_FAIL;
    }

    g_async_queue_push(g_tunnel_work_local->cmd_queue, cmd);
    worker_signal_cmd_event();
    return ERRCODE_SUCCESS;
}

static int worker_handle_candidate(tunnel_worker_cmd_t *cmd, int add)
{
    if (!cmd || !cmd->msg || !g_tunnel_work_local || !g_tunnel_work_local->rib || !cmd->msg->payload ||
        cmd->msg->payload_len < sizeof(tunnel_candidate_t))
    {
        return ERRCODE_FAIL;
    }

    const tunnel_candidate_t *candidate = cmd->msg->payload;
    int rc = add ? tunnel_rib_candidate_add(g_tunnel_work_local->rib, candidate)
                 : tunnel_rib_candidate_del(g_tunnel_work_local->rib, candidate);
    tunnel_rib_recompute(g_tunnel_work_local->rib);
    worker_send_ack(cmd->msg, rc);
    return rc;
}

static int worker_handle_watch(tunnel_worker_cmd_t *cmd, int add)
{
    if (!cmd || !cmd->msg || !g_tunnel_work_local || !g_tunnel_work_local->rib || !cmd->msg->payload ||
        cmd->msg->payload_len < sizeof(tunnel_resolve_req_t))
    {
        return ERRCODE_FAIL;
    }

    const tunnel_resolve_req_t *req = cmd->msg->payload;
    int rc = add ? tunnel_rib_watch_add(g_tunnel_work_local->rib, cmd->msg->src_module_id, req)
                 : tunnel_rib_watch_del(g_tunnel_work_local->rib, cmd->msg->src_module_id, req);
    tunnel_rib_recompute(g_tunnel_work_local->rib);
    worker_send_ack(cmd->msg, rc);
    return rc;
}

static int worker_handle_label_alloc(tunnel_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->msg || !g_tunnel_work_local || !g_tunnel_work_local->rib || !cmd->msg->payload ||
        cmd->msg->payload_len < sizeof(tunnel_label_req_t))
    {
        return ERRCODE_FAIL;
    }

    const tunnel_label_req_t *req = cmd->msg->payload;
    uint32_t label = 0;
    int rc = tunnel_rib_label_alloc(g_tunnel_work_local->rib, req, &label);
    if (rc == ERRCODE_SUCCESS)
    {
        tunnel_rib_recompute(g_tunnel_work_local->rib);
    }
    worker_send_ack_label(cmd->msg, rc, label);
    return rc;
}

static int worker_handle_label_release(tunnel_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->msg || !g_tunnel_work_local || !g_tunnel_work_local->rib || !cmd->msg->payload ||
        cmd->msg->payload_len < sizeof(tunnel_label_req_t))
    {
        return ERRCODE_FAIL;
    }

    const tunnel_label_req_t *req = cmd->msg->payload;
    int rc = tunnel_rib_label_release(g_tunnel_work_local->rib, req);
    if (rc == ERRCODE_SUCCESS)
    {
        tunnel_rib_recompute(g_tunnel_work_local->rib);
    }
    worker_send_ack(cmd->msg, rc);
    return rc;
}

static int worker_dispatch_cmd(tunnel_worker_cmd_t *cmd)
{
    int stop = 0;

    switch (cmd->type)
    {
        case TUNNEL_WORKER_CMD_CANDIDATE_ADD:
            (void)worker_handle_candidate(cmd, 1);
            break;
        case TUNNEL_WORKER_CMD_CANDIDATE_DEL:
            (void)worker_handle_candidate(cmd, 0);
            break;
        case TUNNEL_WORKER_CMD_RESOLVE_REGISTER:
            (void)worker_handle_watch(cmd, 1);
            break;
        case TUNNEL_WORKER_CMD_RESOLVE_UNREGISTER:
            (void)worker_handle_watch(cmd, 0);
            break;
        case TUNNEL_WORKER_CMD_LABEL_ALLOC:
            (void)worker_handle_label_alloc(cmd);
            break;
        case TUNNEL_WORKER_CMD_LABEL_RELEASE:
            (void)worker_handle_label_release(cmd);
            break;
        case TUNNEL_WORKER_CMD_CLI_SHOW:
            (void)tunnel_show_dispatch(cmd->msg);
            break;
        case TUNNEL_WORKER_CMD_SHUTDOWN:
            g_tunnel_work_local->running = 0;
            stop = 1;
            break;
        default:
            LOG_WARN("TUNNEL: unknown worker command %d", (int)cmd->type);
            break;
    }

    worker_cmd_destroy(cmd);
    return stop;
}

static int worker_drain_cmd_queue(void)
{
    uint64_t v;
    while (read(g_tunnel_work_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("TUNNEL: cmd eventfd read failed");
    }

    tunnel_worker_cmd_t *cmd = NULL;
    while ((cmd = g_async_queue_try_pop(g_tunnel_work_local->cmd_queue)) != NULL)
    {
        if (worker_dispatch_cmd(cmd))
        {
            return 1;
        }
    }
    return 0;
}

static void *tunnel_worker_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "tunnel-worker");
    log_set_tag("tunnel");

    tunnel_work_local_t *wl = g_tunnel_work_local;
    struct epoll_event events[TUNNEL_MAX_EPOLL_EVENTS];

    LOG_INFO("TUNNEL: worker thread started");
    while (wl->running)
    {
        int n = epoll_wait(wl->epoll_fd, events, TUNNEL_MAX_EPOLL_EVENTS, 1000);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("TUNNEL: epoll_wait failed");
            break;
        }

        for (int i = 0; i < n; i++)
        {
            if (events[i].data.ptr == &g_tunnel_cmd_tag)
            {
                if (worker_drain_cmd_queue())
                {
                    goto out;
                }
                continue;
            }
            LOG_WARN("TUNNEL: unknown epoll event ptr=%p", events[i].data.ptr);
        }
    }

out:
    LOG_INFO("TUNNEL: worker thread stopped");
    return NULL;
}

int tunnel_worker_prepare(void)
{
    if (!g_tunnel_work_local)
    {
        g_tunnel_work_local = g_malloc0(sizeof(*g_tunnel_work_local));
        if (!g_tunnel_work_local)
        {
            return ERRCODE_FAIL;
        }
        g_tunnel_work_local->epoll_fd = -1;
        g_tunnel_work_local->cmd_eventfd = -1;
        g_tunnel_work_local->rib = tunnel_rib_create();
        if (!g_tunnel_work_local->rib)
        {
            tunnel_worker_shutdown();
            return ERRCODE_FAIL;
        }
    }

    g_tunnel_work_local->running = 1;
    g_tunnel_work_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_tunnel_work_local->epoll_fd < 0)
    {
        LOG_PERROR("TUNNEL: epoll_create1 failed");
        tunnel_worker_shutdown();
        return ERRCODE_FAIL;
    }

    g_tunnel_work_local->cmd_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_tunnel_work_local->cmd_eventfd < 0)
    {
        LOG_PERROR("TUNNEL: eventfd failed");
        tunnel_worker_shutdown();
        return ERRCODE_FAIL;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_tunnel_cmd_tag;
    if (epoll_ctl(g_tunnel_work_local->epoll_fd, EPOLL_CTL_ADD, g_tunnel_work_local->cmd_eventfd, &ev) < 0)
    {
        LOG_PERROR("TUNNEL: epoll_ctl ADD cmd_eventfd failed");
        tunnel_worker_shutdown();
        return ERRCODE_FAIL;
    }

    g_tunnel_work_local->cmd_queue = g_async_queue_new();
    if (!g_tunnel_work_local->cmd_queue)
    {
        tunnel_worker_shutdown();
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int tunnel_worker_launch(void)
{
    if (!g_tunnel_work_local)
    {
        return ERRCODE_FAIL;
    }

    if (pthread_create(&g_tunnel_work_local->thread, NULL, tunnel_worker_thread_fn, NULL) != 0)
    {
        LOG_PERROR("TUNNEL: pthread_create failed");
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int tunnel_worker_post(tunnel_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    tunnel_worker_cmd_t *cmd = worker_cmd_create(type, msg);
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

int tunnel_worker_post_show_cli(dev_ipc_message_t *msg)
{
    return tunnel_worker_post(TUNNEL_WORKER_CMD_CLI_SHOW, msg);
}

void tunnel_worker_shutdown(void)
{
    if (!g_tunnel_work_local)
    {
        return;
    }

    if (g_tunnel_work_local->running && g_tunnel_work_local->thread != 0)
    {
        tunnel_worker_cmd_t *cmd = worker_cmd_create(TUNNEL_WORKER_CMD_SHUTDOWN, NULL);
        if (cmd)
        {
            g_async_queue_push(g_tunnel_work_local->cmd_queue, cmd);
            worker_signal_cmd_event();
        }
        pthread_join(g_tunnel_work_local->thread, NULL);
        g_tunnel_work_local->thread = 0;
    }

    if (g_tunnel_work_local->cmd_queue)
    {
        tunnel_worker_cmd_t *cmd = NULL;
        while ((cmd = g_async_queue_try_pop(g_tunnel_work_local->cmd_queue)) != NULL)
        {
            worker_cmd_destroy(cmd);
        }
        g_async_queue_unref(g_tunnel_work_local->cmd_queue);
        g_tunnel_work_local->cmd_queue = NULL;
    }

    if (g_tunnel_work_local->epoll_fd >= 0)
    {
        close(g_tunnel_work_local->epoll_fd);
        g_tunnel_work_local->epoll_fd = -1;
    }

    if (g_tunnel_work_local->cmd_eventfd >= 0)
    {
        close(g_tunnel_work_local->cmd_eventfd);
        g_tunnel_work_local->cmd_eventfd = -1;
    }

    tunnel_show_cleanup_state();

    if (g_tunnel_work_local->rib)
    {
        tunnel_rib_destroy(g_tunnel_work_local->rib);
        g_tunnel_work_local->rib = NULL;
    }

    g_free(g_tunnel_work_local);
    g_tunnel_work_local = NULL;
}
