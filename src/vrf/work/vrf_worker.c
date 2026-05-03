/**
 * @file   vrf_worker.c
 * @brief  VRF worker 线程实现
 * @author jhb
 * @date   2026/05/02
 */
#include "vrf_worker.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "log.h"
#include "vrf_db.h"
#include "vrf_main.h"
#include "vrf_pub.h"
#include "vrf_show.h"

vrf_work_local_t *g_vrf_work_local = NULL;

// ============================================================================
// worker 命令
// ============================================================================

typedef enum vrf_worker_cmd_type
{
    VRF_WORKER_CMD_IPC_MSG = 1,
    VRF_WORKER_CMD_APPLY = 2,
    VRF_WORKER_CMD_DB_RESTORE = 3,
    VRF_WORKER_CMD_SHUTDOWN = 4,
} vrf_worker_cmd_type_t;

typedef struct vrf_worker_cmd
{
    vrf_worker_cmd_type_t type;
    dev_ipc_message_t *msg;
    vrf_apply_cmd_t *apply;

    int waitable;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int rc;
} vrf_worker_cmd_t;

static vrf_worker_cmd_t *cmd_create(vrf_worker_cmd_type_t type, int waitable)
{
    vrf_worker_cmd_t *c = g_malloc0(sizeof(*c));
    c->type = type;
    c->waitable = waitable;
    if (waitable)
    {
        pthread_mutex_init(&c->mutex, NULL);
        pthread_cond_init(&c->cond, NULL);
    }
    return c;
}

static void cmd_destroy(vrf_worker_cmd_t *c)
{
    if (!c)
    {
        return;
    }
    if (c->waitable)
    {
        pthread_mutex_destroy(&c->mutex);
        pthread_cond_destroy(&c->cond);
    }
    g_free(c);
}

static void cmd_complete(vrf_worker_cmd_t *c, int rc)
{
    if (!c || !c->waitable)
    {
        return;
    }
    pthread_mutex_lock(&c->mutex);
    c->rc = rc;
    c->done = 1;
    pthread_cond_signal(&c->cond);
    pthread_mutex_unlock(&c->mutex);
}

static int cmd_wait(vrf_worker_cmd_t *c)
{
    if (!c || !c->waitable)
    {
        return ERRCODE_FAIL;
    }
    pthread_mutex_lock(&c->mutex);
    while (!c->done)
    {
        pthread_cond_wait(&c->cond, &c->mutex);
    }
    int rc = c->rc;
    pthread_mutex_unlock(&c->mutex);
    return rc;
}

static int cmd_enqueue(vrf_worker_cmd_t *c)
{
    if (!c || !g_vrf_work_local || !g_vrf_work_local->cmd_queue || !g_vrf_work_local->running)
    {
        return ERRCODE_FAIL;
    }
    g_async_queue_push(g_vrf_work_local->cmd_queue, c);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// IPC 消息处理（worker 线程）
// ============================================================================

static void worker_handle_ipc_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    switch (msg->msg_type)
    {
        case CLI_MSG_TYPE:
            (void)vrf_show_handle_cli(msg);
            dev_ipc_message_free(msg);
            return;
        case CLI_MSG_TYPE_CONTINUE:
            (void)vrf_show_handle_continue(msg);
            dev_ipc_message_free(msg);
            return;
        case CLI_MSG_TYPE_QUERY_CANDIDATES:
            vrf_show_handle_query_candidates(msg);
            return;
        case VRF_MSG_TYPE_SUBSCRIBE:
            vrf_pub_handle_subscribe(msg);
            return;
        case VRF_MSG_TYPE_UNSUBSCRIBE:
            vrf_pub_handle_unsubscribe(msg);
            return;
        default:
            LOG_WARN("VRF-WORKER: unexpected msg 0x%08X", msg->msg_type);
            dev_ipc_message_free(msg);
            return;
    }
}

static void *worker_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "vrf-worker");
    log_set_tag("vrf");

    /* 公网 VRF：worker 启动即装入，确保订阅前就在表里 */
    (void)vrf_table_create_with_id(&g_vrf_work_local->table, VRF_PUBLIC_VRF_ID, VRF_PUBLIC_VRF_NAME, 0);

    while (g_vrf_work_local && g_vrf_work_local->running)
    {
        vrf_worker_cmd_t *c = (vrf_worker_cmd_t *)g_async_queue_pop(g_vrf_work_local->cmd_queue);
        if (!c)
        {
            continue;
        }
        switch (c->type)
        {
            case VRF_WORKER_CMD_IPC_MSG:
                worker_handle_ipc_msg(c->msg);
                c->msg = NULL;
                break;
            case VRF_WORKER_CMD_APPLY:
                if (c->apply)
                {
                    c->apply->rc = vrf_cfg_apply(c->apply);
                }
                cmd_complete(c, ERRCODE_SUCCESS);
                continue; /* waitable 由派发方回收 */
            case VRF_WORKER_CMD_DB_RESTORE:
                (void)vrf_db_load_snapshot();
                break;
            case VRF_WORKER_CMD_SHUTDOWN:
                g_vrf_work_local->running = 0;
                break;
            default:
                if (c->msg)
                {
                    dev_ipc_message_free(c->msg);
                }
                break;
        }
        cmd_destroy(c);
    }
    return NULL;
}

// ============================================================================
// 公开接口
// ============================================================================

int vrf_worker_prepare(void)
{
    if (g_vrf_work_local)
    {
        return ERRCODE_SUCCESS;
    }
    g_vrf_work_local = g_malloc0(sizeof(*g_vrf_work_local));
    vrf_table_init(&g_vrf_work_local->table);
    g_vrf_work_local->cmd_queue = g_async_queue_new();
    g_vrf_work_local->running = 0;
    return ERRCODE_SUCCESS;
}

int vrf_worker_launch(void)
{
    if (!g_vrf_work_local || g_vrf_work_local->running)
    {
        return ERRCODE_FAIL;
    }
    g_vrf_work_local->running = 1;
    if (pthread_create(&g_vrf_work_local->thread, NULL, worker_thread_fn, NULL) != 0)
    {
        g_vrf_work_local->running = 0;
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

void vrf_worker_shutdown(void)
{
    if (!g_vrf_work_local)
    {
        return;
    }
    if (g_vrf_work_local->running)
    {
        vrf_worker_cmd_t *c = cmd_create(VRF_WORKER_CMD_SHUTDOWN, 0);
        g_async_queue_push(g_vrf_work_local->cmd_queue, c);
        pthread_join(g_vrf_work_local->thread, NULL);
    }
    if (g_vrf_work_local->subscribers)
    {
        g_list_free_full(g_vrf_work_local->subscribers, g_free);
        g_vrf_work_local->subscribers = NULL;
    }
    if (g_vrf_work_local->cmd_queue)
    {
        g_async_queue_unref(g_vrf_work_local->cmd_queue);
        g_vrf_work_local->cmd_queue = NULL;
    }
    cli_chunk_stream_reset(&g_vrf_work_local->show_stream);
    vrf_table_destroy(&g_vrf_work_local->table);
    g_free(g_vrf_work_local);
    g_vrf_work_local = NULL;
}

int vrf_worker_post_ipc_message(dev_ipc_message_t *msg)
{
    vrf_worker_cmd_t *c = cmd_create(VRF_WORKER_CMD_IPC_MSG, 0);
    c->msg = msg;
    if (cmd_enqueue(c) != ERRCODE_SUCCESS)
    {
        cmd_destroy(c);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int vrf_worker_dispatch_apply(vrf_apply_cmd_t *apply)
{
    if (!apply)
    {
        return ERRCODE_FAIL;
    }
    vrf_worker_cmd_t *c = cmd_create(VRF_WORKER_CMD_APPLY, 1);
    c->apply = apply;
    if (cmd_enqueue(c) != ERRCODE_SUCCESS)
    {
        cmd_destroy(c);
        return ERRCODE_FAIL;
    }
    int rc = cmd_wait(c);
    cmd_destroy(c);
    return rc;
}

int vrf_worker_dispatch_db_restore(void)
{
    vrf_worker_cmd_t *c = cmd_create(VRF_WORKER_CMD_DB_RESTORE, 0);
    if (cmd_enqueue(c) != ERRCODE_SUCCESS)
    {
        cmd_destroy(c);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

dev_ipc_context_t *vrf_worker_ipc_ctx(void)
{
    return vrf_local_ipc_ctx();
}

vrf_table_t *vrf_worker_table(void)
{
    return g_vrf_work_local ? &g_vrf_work_local->table : NULL;
}

GList *vrf_worker_subscribers(void)
{
    return g_vrf_work_local ? g_vrf_work_local->subscribers : NULL;
}

GList **vrf_worker_subscribers_ptr(void)
{
    return g_vrf_work_local ? &g_vrf_work_local->subscribers : NULL;
}

cli_chunk_stream_t *vrf_worker_show_stream(void)
{
    return g_vrf_work_local ? &g_vrf_work_local->show_stream : NULL;
}
