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
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_VRF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: VRF not connected in time; vrf_api_subscribe deferred");
        return;
    }
    if (vrf_api_subscribe_vrf(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: vrf_api_subscribe_vrf failed");
    }
    else
    {
        LOG_INFO("FIB: subscribed to VRF lifecycle events");
    }
}

/**
 * FIB 本地 init（合并原 on_start/on_ready 逻辑）。
 */
static int fib_init_local(void)
{
    dev_ipc_context_t *ctx = fib_local_ipc_ctx();

    /* fib_worker_prepare() 必须在 dev_ipc_init() 开放监听端口前完成。
     * STARTING 模块允许其它模块提前建联；DEV 尚未连接时，ROUTE 已可能下发
     * 初始路由。此处只负责启动预先建好的 worker，积压消息随后按序处理。 */
    if (fib_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("FIB: worker start failed");
        fib_worker_shutdown();
        return -1;
    }

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("FIB: timed out waiting for DEV connection");
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: notify_ready to DEV failed");
    }

    /* VRF（常驻）：auto_start=0；cb 在 VRF READY（含重启）时触发 VRF lifecycle 订阅。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_VRF, 0, fib_on_vrf_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: subscribe(VRF) failed");
    }

    /* CLI 末尾 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: subscribe(CLI) failed");
    }

    LOG_INFO("FIB: module ready");
    return 0;
}

static void post_or_free(fib_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    if (fib_worker_post(type, msg) != ERRCODE_SUCCESS)
    {
        LOG_WARN("FIB: failed to post worker command %d", (int)type);
        switch (type)
        {
            case FIB_WORKER_CMD_ROUTE_UPSERT:
            case FIB_WORKER_CMD_ROUTE_DELETE:
            case FIB_WORKER_CMD_TUNNEL_UPSERT:
            case FIB_WORKER_CMD_TUNNEL_DELETE:
            case FIB_WORKER_CMD_ILM_UPSERT:
            case FIB_WORKER_CMD_ILM_DELETE:
            case FIB_WORKER_CMD_NEXTHOP_UPSERT:
            case FIB_WORKER_CMD_NEXTHOP_DELETE:
            case FIB_WORKER_CMD_SRV6_LOCALSID_UPSERT:
            case FIB_WORKER_CMD_SRV6_LOCALSID_DELETE:
                fib_worker_send_ack(msg, ERRCODE_FAIL);
                break;
            case FIB_WORKER_CMD_SHOW_CLI:
            case FIB_WORKER_CMD_SHUTDOWN:
            case FIB_WORKER_CMD_VRF_EVENT:
                break;
        }
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
        case FIB_MSG_TYPE_NEXTHOP_UPSERT:
            post_or_free(FIB_WORKER_CMD_NEXTHOP_UPSERT, msg);
            return;
        case FIB_MSG_TYPE_NEXTHOP_DELETE:
            post_or_free(FIB_WORKER_CMD_NEXTHOP_DELETE, msg);
            return;
        case FIB_MSG_TYPE_SRV6_LOCALSID_UPSERT:
            post_or_free(FIB_WORKER_CMD_SRV6_LOCALSID_UPSERT, msg);
            return;
        case FIB_MSG_TYPE_SRV6_LOCALSID_DELETE:
            post_or_free(FIB_WORKER_CMD_SRV6_LOCALSID_DELETE, msg);
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

    /* 先创建 worker 队列，再开放 IPC listener。模块处于 STARTING 时其它 peer
     * 可以主动连接；队列必须已经存在，才能无损缓存 READY 前到达的业务消息。 */
    if (fib_worker_prepare() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("FIB: worker prepare failed");
        fib_worker_shutdown();
        return -1;
    }

    g_fib_local = g_malloc0(sizeof(*g_fib_local));
    if (!g_fib_local)
    {
        fib_worker_shutdown();
        return -1;
    }

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_FIB, "fib", DEV_MODULE_PORT_FIB, fib_ipc_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        g_free(g_fib_local);
        g_fib_local = NULL;
        fib_worker_shutdown();
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
    }

    fib_worker_shutdown();

    /* 向 DEV 发 PRE_EXIT 通知，等 DEV 同步完成 phase/broadcast/drop 后再 ACK。
     * 必须在 dev_ipc_destroy 之前；超时/失败不阻塞退出，SIGCHLD 路径仍会兜底清理。 */
    if (ctx)
    {
        dev_ipc_pre_exit_notify(ctx, 3000);
    }

    if (g_fib_local)
    {
        g_fib_local->dev_ipc_ctx = NULL;
    }

    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }
    g_free(g_fib_local);
    g_fib_local = NULL;
}
