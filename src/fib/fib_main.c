#include "fib_main.h"

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "fib.h"
#include "fib_worker.h"
#include "log.h"
#include "vrf.h"

fib_local_t *g_fib_local = NULL;

static void send_empty_show_config_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_FIB, msg->src_module_id,
                                                     msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
}

/* VRF dep 就绪回调（含初次 + 重启）。
 * IO/同步上下文，不阻塞——投递 worker 内部消息让 worker 线程做实际订阅。 */
static void fib_on_vrf_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event != DEV_MODULE_EVENT_READY)
    {
        return;
    }
    if (!g_fib_local || !g_fib_local->dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m =
        dev_ipc_message_create(FIB_MSG_TYPE_INTERNAL_VRF_READY, DEV_MODULE_ID_FIB, DEV_MODULE_ID_FIB, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_fib_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void fib_handle_vrf_ready(void)
{
    dev_ipc_context_t *ctx = fib_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_VRF, 3000) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: VRF not connected within 3s; vrf_api_subscribe deferred");
        return;
    }
    if (vrf_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: vrf_api_subscribe_all failed");
    }
    else
    {
        LOG_INFO("FIB: subscribed to VRF events");
    }
}

/**
 * FIB 本地 init（合并原 on_start/on_ready 逻辑）。
 */
static int fib_init_local(void)
{
    dev_ipc_context_t *ctx = fib_local_ipc_ctx();

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, 10000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("FIB: timed out waiting for DEV connection");
    }

    if (fib_worker_prepare() != ERRCODE_SUCCESS || fib_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("FIB: worker start failed");
        fib_worker_shutdown();
        return -1;
    }

    /* VRF（on-demand）：auto_start=1 + cb 重启感知 */
    /* VRF 用 auto_start=0：FIB 不硬依赖 VRF；cb 在 VRF 实际启动后才触发 vrf_api_subscribe_all */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_VRF, 0, fib_on_vrf_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: subscribe(VRF) failed");
    }

    /* CLI 末尾 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: subscribe(CLI) failed");
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: notify_ready to DEV failed");
    }
    LOG_INFO("FIB: module ready");
    return 0;
}

static void post_or_free(fib_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    if (fib_worker_post(type, msg) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: failed to post worker command %d", (int)type);
        dev_ipc_message_free(msg);
    }
}

static uint8_t fib_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

void fib_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case FIB_MSG_TYPE_INTERNAL_VRF_READY:
            fib_handle_vrf_ready();
            break;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            send_empty_show_config_response(ctx, msg);
            return;
        case CLI_MSG_TYPE:
            if ((fib_cli_payload_flags(msg) & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                post_or_free(FIB_WORKER_CMD_SHOW_CLI, msg);
                return;
            }
            break;
        case CLI_MSG_TYPE_CONTINUE:
            post_or_free(FIB_WORKER_CMD_SHOW_CLI, msg);
            return;
        case FIB_MSG_TYPE_ROUTE_UPSERT:
            post_or_free(FIB_WORKER_CMD_ROUTE_UPSERT, msg);
            return;
        case FIB_MSG_TYPE_ROUTE_DELETE:
            post_or_free(FIB_WORKER_CMD_ROUTE_DELETE, msg);
            return;
        case FIB_MSG_TYPE_TUNNEL_UPSERT:
            post_or_free(FIB_WORKER_CMD_TUNNEL_UPSERT, msg);
            return;
        case FIB_MSG_TYPE_TUNNEL_DELETE:
            post_or_free(FIB_WORKER_CMD_TUNNEL_DELETE, msg);
            return;
        case FIB_MSG_TYPE_ILM_UPSERT:
            post_or_free(FIB_WORKER_CMD_ILM_UPSERT, msg);
            return;
        case FIB_MSG_TYPE_ILM_DELETE:
            post_or_free(FIB_WORKER_CMD_ILM_DELETE, msg);
            return;
        case VRF_MSG_TYPE_EVENT:
            post_or_free(FIB_WORKER_CMD_VRF_EVENT, msg);
            return;
        case VRF_MSG_TYPE_ACK:
            break;
        default:
            LOG_WARN("FIB: unknown message type 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

int fib_module_init(void)
{
    log_set_tag("fib");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_FIB, "fib", DEV_MODULE_PORT_FIB, fib_ipc_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_fib_local = g_malloc0(sizeof(*g_fib_local));
    if (!g_fib_local)
    {
        dev_ipc_destroy(ctx);
        return -1;
    }
    g_fib_local->dev_ipc_ctx = ctx;
    return fib_init_local();
}

void fib_module_cleanup(void)
{
    dev_ipc_context_t *ctx = NULL;
    if (g_fib_local)
    {
        ctx = g_fib_local->dev_ipc_ctx;
        g_fib_local->dev_ipc_ctx = NULL;
    }

    fib_worker_shutdown();
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }
    g_free(g_fib_local);
    g_fib_local = NULL;
}
