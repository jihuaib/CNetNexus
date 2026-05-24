#include "tunnel_main.h"

#include <glib.h>

#include "cli.h"
#include "errcode.h"
#include "log.h"
#include "tunnel.h"
#include "tunnel_cli.h"
#include "tunnel_worker.h"

tunnel_local_t *g_tunnel_local = NULL;

static uint8_t tunnel_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

static void send_empty_show_config_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_TUNNEL, msg->src_module_id,
                                                     msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
}

void tunnel_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case CLI_MSG_TYPE_SHOW_CONFIG:
            send_empty_show_config_response(ctx, msg);
            return;

        case CLI_MSG_TYPE:
            if ((tunnel_cli_payload_flags(msg) & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (tunnel_cli_handle_show(msg) != ERRCODE_SUCCESS)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                dev_ipc_message_free(msg);
            }
            return;

        case CLI_MSG_TYPE_CONTINUE:
            if (tunnel_cli_handle_continue(msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;

        case TUNNEL_MSG_TYPE_CANDIDATE_ADD:
            if (tunnel_worker_post(TUNNEL_WORKER_CMD_CANDIDATE_ADD, msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;
        case TUNNEL_MSG_TYPE_CANDIDATE_DEL:
            if (tunnel_worker_post(TUNNEL_WORKER_CMD_CANDIDATE_DEL, msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;
        case TUNNEL_MSG_TYPE_RESOLVE_REGISTER:
            if (tunnel_worker_post(TUNNEL_WORKER_CMD_RESOLVE_REGISTER, msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;
        case TUNNEL_MSG_TYPE_RESOLVE_UNREGISTER:
            if (tunnel_worker_post(TUNNEL_WORKER_CMD_RESOLVE_UNREGISTER, msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;
        case TUNNEL_MSG_TYPE_RESOLVE_QUERY:
            if (tunnel_worker_post(TUNNEL_WORKER_CMD_RESOLVE_QUERY, msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;
        case TUNNEL_MSG_TYPE_LABEL_ALLOC:
            if (tunnel_worker_post(TUNNEL_WORKER_CMD_LABEL_ALLOC, msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;
        case TUNNEL_MSG_TYPE_LABEL_RELEASE:
            if (tunnel_worker_post(TUNNEL_WORKER_CMD_LABEL_RELEASE, msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;

        default:
            dev_ipc_message_free(msg);
            return;
    }
}

int tunnel_module_init(void)
{
    log_set_tag("tunnel");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_TUNNEL, "tunnel", DEV_MODULE_PORT_TUNNEL, tunnel_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("TUNNEL: IPC initialization failed");
        return -1;
    }

    g_tunnel_local = g_malloc0(sizeof(*g_tunnel_local));
    if (!g_tunnel_local)
    {
        dev_ipc_destroy(ctx);
        return -1;
    }

    g_tunnel_local->dev_ipc_ctx = ctx;

    /* 弱依赖模型：
     *   1. 等 DEV 控制连接；
     *   2. 启动本地 worker（本地状态就绪）；
     *   3. 最后才 subscribe(CLI)：让 CFG is_connected 时本模块已完全可服务；
     *   4. notify_ready 告知 DEV（DEV 把 READY 推给订阅者如 BGP/LDP）。
     *   其它 dep (ROUTE/IF/FIB) 运行时 RPC 调用即可。 */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, 10000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("TUNNEL: timed out waiting for DEV connection; module may be unusable");
    }

    if (tunnel_worker_prepare() != ERRCODE_SUCCESS || tunnel_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("TUNNEL: worker start failed");
        tunnel_worker_shutdown();
        return -1;
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("TUNNEL: notify_ready to DEV failed");
    }

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("TUNNEL: subscribe(CLI) failed; commands from CFG won't be reachable");
    }

    LOG_INFO("TUNNEL: module ready");

    return 0;
}

void tunnel_module_cleanup(void)
{
    if (!g_tunnel_local)
    {
        return;
    }

    tunnel_worker_shutdown();

    dev_ipc_context_t *ctx = g_tunnel_local->dev_ipc_ctx;
    g_tunnel_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_tunnel_local);
    g_tunnel_local = NULL;
}
