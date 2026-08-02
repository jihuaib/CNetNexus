#include "srv6_main.h"

#include <stddef.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "log.h"
#include "srv6.h"
#include "srv6_bdr.h"
#include "srv6_cli.h"
#include "srv6_db.h"
#include "vrf.h"
#include "work/srv6_worker.h"

srv6_local_t *g_srv6_local;

static void srv6_post_internal(uint32_t msg_type)
{
    if (!g_srv6_local || !g_srv6_local->dev_ipc_ctx || g_srv6_local->shutting_down)
    {
        return;
    }
    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, DEV_MODULE_ID_SRV6, DEV_MODULE_ID_SRV6, 0, NULL, 0, NULL);
    if (msg)
    {
        g_async_queue_push(g_srv6_local->dev_ipc_ctx->msg_queue, msg);
    }
}

static void srv6_on_db_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                             void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        srv6_post_internal(SRV6_MSG_TYPE_INTERNAL_DB_READY);
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        srv6_post_internal(SRV6_MSG_TYPE_INTERNAL_DB_DOWN);
    }
}

static void srv6_on_vrf_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                              void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        srv6_post_internal(SRV6_MSG_TYPE_INTERNAL_VRF_READY);
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        srv6_post_internal(SRV6_MSG_TYPE_INTERNAL_VRF_DOWN);
    }
}

static void srv6_on_fib_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                              void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        srv6_post_internal(SRV6_MSG_TYPE_INTERNAL_FIB_READY);
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        srv6_post_internal(SRV6_MSG_TYPE_INTERNAL_FIB_DOWN);
    }
}

static void srv6_on_route_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        srv6_post_internal(SRV6_MSG_TYPE_INTERNAL_ROUTE_READY);
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        srv6_post_internal(SRV6_MSG_TYPE_INTERNAL_ROUTE_DOWN);
    }
}

static void srv6_finish_restore(int rc)
{
    pthread_mutex_lock(&g_srv6_local->startup_mutex);
    g_srv6_local->restore_done = TRUE;
    g_srv6_local->restore_rc = rc;
    pthread_cond_broadcast(&g_srv6_local->startup_cond);
    pthread_mutex_unlock(&g_srv6_local->startup_mutex);
}

static void srv6_try_restore(void)
{
    pthread_mutex_lock(&g_srv6_local->startup_mutex);
    gboolean should_restore = !g_srv6_local->restore_done && g_srv6_local->db_ready && g_srv6_local->fib_ready &&
                              g_srv6_local->route_ready && g_srv6_local->vrf_smoothend;
    pthread_mutex_unlock(&g_srv6_local->startup_mutex);
    if (!should_restore)
    {
        return;
    }
    int rc = srv6_db_restore();
    if (rc != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: persisted state restore failed");
    }
    else
    {
        LOG_INFO("SRV6: persisted state restore complete");
    }
    srv6_finish_restore(rc);
}

static void srv6_handle_db_ready(void)
{
    dev_ipc_context_t *ctx = srv6_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SRV6: DB connection not ready");
        return;
    }
    if (srv6_db_init() != ERRCODE_SUCCESS)
    {
        srv6_finish_restore(ERRCODE_FAIL);
        return;
    }
    pthread_mutex_lock(&g_srv6_local->startup_mutex);
    g_srv6_local->db_ready = TRUE;
    pthread_mutex_unlock(&g_srv6_local->startup_mutex);
    srv6_try_restore();
}

static void srv6_handle_vrf_ready(void)
{
    dev_ipc_context_t *ctx = srv6_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_VRF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SRV6: VRF connection not ready");
        return;
    }
    pthread_mutex_lock(&g_srv6_local->startup_mutex);
    g_srv6_local->vrf_smoothend = FALSE;
    pthread_mutex_unlock(&g_srv6_local->startup_mutex);
    if (vrf_api_subscribe(ctx, VRF_AF_MASK_ALL, VRF_EVENT_ALL, VRF_SUBSCRIBE_FLAG_REPLAY) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SRV6: VRF event subscribe failed");
    }
}

static void srv6_handle_fib_ready(void)
{
    if (dev_ipc_wait_connected(srv6_local_ipc_ctx(), DEV_MODULE_ID_FIB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SRV6: FIB connection not ready");
        return;
    }
    (void)srv6_worker_post_fib_ready();
    pthread_mutex_lock(&g_srv6_local->startup_mutex);
    g_srv6_local->fib_ready = TRUE;
    pthread_mutex_unlock(&g_srv6_local->startup_mutex);
    srv6_try_restore();
}

static void srv6_handle_route_ready(void)
{
    if (dev_ipc_wait_connected(srv6_local_ipc_ctx(), DEV_MODULE_ID_ROUTE, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SRV6: ROUTE connection not ready");
        return;
    }
    if (srv6_worker_post_route_ready() != ERRCODE_SUCCESS)
    {
        pthread_mutex_lock(&g_srv6_local->startup_mutex);
        g_srv6_local->route_ready = FALSE;
        pthread_mutex_unlock(&g_srv6_local->startup_mutex);
        LOG_WARN("SRV6: failed to post ROUTE-ready replay");
        return;
    }
    pthread_mutex_lock(&g_srv6_local->startup_mutex);
    g_srv6_local->route_ready = TRUE;
    pthread_mutex_unlock(&g_srv6_local->startup_mutex);
    srv6_try_restore();
}

static uint8_t srv6_cli_flags(const dev_ipc_message_t *msg)
{
    return msg && msg->payload && msg->payload_len > 0u ? ((const uint8_t *)msg->payload)[0] : 0u;
}

static void srv6_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    if (!g_srv6_local || g_srv6_local->shutting_down)
    {
        dev_ipc_message_free(msg);
        return;
    }

    switch (msg->msg_type)
    {
        case SRV6_MSG_TYPE_INTERNAL_DB_READY:
            srv6_handle_db_ready();
            break;
        case SRV6_MSG_TYPE_INTERNAL_DB_DOWN:
            pthread_mutex_lock(&g_srv6_local->startup_mutex);
            g_srv6_local->db_ready = FALSE;
            pthread_mutex_unlock(&g_srv6_local->startup_mutex);
            break;
        case SRV6_MSG_TYPE_INTERNAL_VRF_READY:
            srv6_handle_vrf_ready();
            break;
        case SRV6_MSG_TYPE_INTERNAL_VRF_DOWN:
            pthread_mutex_lock(&g_srv6_local->startup_mutex);
            g_srv6_local->vrf_smoothend = FALSE;
            pthread_mutex_unlock(&g_srv6_local->startup_mutex);
            (void)srv6_worker_post_vrf_down();
            break;
        case SRV6_MSG_TYPE_INTERNAL_FIB_READY:
            srv6_handle_fib_ready();
            break;
        case SRV6_MSG_TYPE_INTERNAL_FIB_DOWN:
            pthread_mutex_lock(&g_srv6_local->startup_mutex);
            g_srv6_local->fib_ready = FALSE;
            pthread_mutex_unlock(&g_srv6_local->startup_mutex);
            (void)srv6_worker_post_fib_down();
            break;
        case SRV6_MSG_TYPE_INTERNAL_ROUTE_READY:
            srv6_handle_route_ready();
            break;
        case SRV6_MSG_TYPE_INTERNAL_ROUTE_DOWN:
            pthread_mutex_lock(&g_srv6_local->startup_mutex);
            g_srv6_local->route_ready = FALSE;
            pthread_mutex_unlock(&g_srv6_local->startup_mutex);
            (void)srv6_worker_post_route_down();
            break;

        case SRV6_MSG_TYPE_SID_ALLOC:
        case SRV6_MSG_TYPE_SID_RELEASE:
        case SRV6_MSG_TYPE_SID_RELEASE_OWNER:
        case SRV6_MSG_TYPE_SID_GET:
        case SRV6_MSG_TYPE_LOCATOR_GET:
            if (srv6_worker_post_rpc(msg) != ERRCODE_SUCCESS)
            {
                srv6_worker_send_rpc_failure(msg);
                dev_ipc_message_free(msg);
            }
            return;

        case CLI_MSG_TYPE:
            if ((srv6_cli_flags(msg) & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0u)
            {
                if (srv6_worker_post_show(msg) != ERRCODE_SUCCESS)
                {
                    dev_ipc_message_free(msg);
                }
                return;
            }
            if (db_rpc_guard_reject(ctx, msg, "SRV6"))
            {
                dev_ipc_message_free(msg);
                return;
            }
            (void)srv6_cli_handle_config_msg(msg);
            break;

        case CLI_MSG_TYPE_CONTINUE:
            if (srv6_worker_post_show(msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)srv6_bdr_handle_show_config(msg);
            break;

        case VRF_MSG_TYPE_EVENT:
        {
            uint32_t event = 0u;
            if (msg->payload && msg->payload_len >= offsetof(vrf_event_msg_t, rts))
            {
                event = ((const vrf_event_msg_t *)msg->payload)->event;
            }
            if (srv6_worker_post_vrf_event(msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
                return;
            }
            if (event == VRF_EVENT_SMOOTHEND)
            {
                pthread_mutex_lock(&g_srv6_local->startup_mutex);
                g_srv6_local->vrf_smoothend = TRUE;
                pthread_mutex_unlock(&g_srv6_local->startup_mutex);
                srv6_try_restore();
            }
            return;
        }
        case VRF_MSG_TYPE_ACK:
            break;
        default:
            LOG_WARN("SRV6: unknown message type 0x%08X", msg->msg_type);
            break;
    }
    dev_ipc_message_free(msg);
}

int srv6_module_init(void)
{
    log_set_tag("srv6");
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_SRV6, "srv6", DEV_MODULE_PORT_SRV6, srv6_msg_handler);
    if (!ctx)
    {
        return -1;
    }
    g_srv6_local = g_new0(srv6_local_t, 1);
    g_srv6_local->dev_ipc_ctx = ctx;
    g_srv6_local->restore_rc = ERRCODE_FAIL;
    pthread_mutex_init(&g_srv6_local->startup_mutex, NULL);
    pthread_cond_init(&g_srv6_local->startup_cond, NULL);

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: timed out waiting for DEV connection");
    }
    if (srv6_worker_prepare() != ERRCODE_SUCCESS || srv6_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: worker start failed");
        srv6_module_cleanup();
        return -1;
    }

    gboolean subscriptions_ok = TRUE;
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, srv6_on_db_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: DB subscription failed");
        subscriptions_ok = FALSE;
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_VRF, 0, srv6_on_vrf_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: VRF subscription failed");
        subscriptions_ok = FALSE;
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_FIB, 0, srv6_on_fib_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: FIB subscription failed");
        subscriptions_ok = FALSE;
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0, srv6_on_route_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: ROUTE subscription failed");
        subscriptions_ok = FALSE;
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: CLI subscription failed");
        subscriptions_ok = FALSE;
    }
    if (!subscriptions_ok)
    {
        srv6_module_cleanup();
        return -1;
    }
    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0);

    pthread_mutex_lock(&g_srv6_local->startup_mutex);
    while (!g_srv6_local->restore_done)
    {
        pthread_cond_wait(&g_srv6_local->startup_cond, &g_srv6_local->startup_mutex);
    }
    int restore_rc = g_srv6_local->restore_rc;
    pthread_mutex_unlock(&g_srv6_local->startup_mutex);
    if (restore_rc != ERRCODE_SUCCESS)
    {
        srv6_module_cleanup();
        return -1;
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SRV6: notify_ready failed");
    }
    LOG_INFO("SRV6: module ready");
    return 0;
}

void srv6_module_cleanup(void)
{
    if (!g_srv6_local)
    {
        return;
    }
    g_srv6_local->shutting_down = TRUE;
    srv6_worker_shutdown();

    dev_ipc_context_t *ctx = g_srv6_local->dev_ipc_ctx;
    if (ctx)
    {
        (void)dev_ipc_pre_exit_notify(ctx, 3000);
        g_srv6_local->dev_ipc_ctx = NULL;
        dev_ipc_destroy(ctx);
    }
    pthread_cond_destroy(&g_srv6_local->startup_cond);
    pthread_mutex_destroy(&g_srv6_local->startup_mutex);
    g_free(g_srv6_local);
    g_srv6_local = NULL;
}
