#include "fib_main.h"

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "fib.h"
#include "fib_worker.h"
#include "log.h"

fib_local_t *g_fib_local = NULL;

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_FIB,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
    (void)result;
}

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

static void fib_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = fib_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - preparing FIB worker");

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI) != 0)
    {
        LOG_WARN("FIB: failed to connect to CLI module");
    }

    if (fib_worker_prepare() != ERRCODE_SUCCESS || fib_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("FIB worker startup failed");
        fib_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

static void fib_on_connect(dev_ipc_message_t *msg)
{
    send_phase_response(fib_local_ipc_ctx(), msg, ERRCODE_SUCCESS);
}

static void fib_on_ready(dev_ipc_message_t *msg)
{
    send_phase_response(fib_local_ipc_ctx(), msg, ERRCODE_SUCCESS);
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
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            fib_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            fib_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            fib_on_ready(msg);
            return;
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
    return 0;
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
