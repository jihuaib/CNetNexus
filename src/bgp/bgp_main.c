/**
 * @file   bgp_main.c
 * @brief  BGP 模块主入口：生命周期处理与消息分发
 * @author jhb
 * @date   2026/01/22
 */
#include "bgp_main.h"

#include "bgp.h"
#include "bgp_bdr.h"
#include "bgp_cli.h"
#include "bgp_db.h"
#include "bgp_pkt.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "route.h"

bgp_local_t *g_bgp_local;

static uint8_t bgp_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_BGP,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(msg);
    (void)result;
}

static void bgp_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");
    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    dev_ipc_connect(ctx, DEV_MODULE_ID_ROUTE, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_ROUTE);
    LOG_INFO("Connected to CFG, DB and ROUTE");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

static void bgp_on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

static void bgp_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Initializing database tables and restoring BGP state");

    if (bgp_db_init() != 0)
    {
        LOG_ERROR("BGP: Database table initialization failed");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    if (bgp_worker_prepare() != ERRCODE_SUCCESS)
    {
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    /* bgp_worker 线程必须先启动，restore 通过 bgp_worker_dispatch_apply() 向其派发命令 */
    if (bgp_worker_launch() != ERRCODE_SUCCESS)
    {
        bgp_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    /* 仅恢复：表不存在（BGP 未曾配置）时静默返回 NULL，不建表也不写默认值 */
    uint32_t ret = bgp_db_restore();
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: Failed to restore state from database");
        bgp_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    LOG_INFO("BGP worker thread started");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

static void bgp_on_shutdown(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    LOG_INFO("BGP module cleanup");

    if (g_bgp_local)
    {
        /* 必须先 join server 线程，再清理 show_stream，避免 worker/server 线程并发访问 */
        bgp_worker_shutdown();
        bgp_cli_cleanup_state();
        g_bgp_local->dev_ipc_ctx = NULL;
        g_free(g_bgp_local);
        g_bgp_local = NULL;
    }

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

void bgp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            bgp_on_start(msg);
            return;

        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            bgp_on_connect(msg);
            return;

        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            bgp_on_ready(msg);
            return;

        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            bgp_on_shutdown(msg);
            return;
        case CLI_MSG_TYPE:
        {
            uint8_t flags = bgp_cli_payload_flags(msg);
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (bgp_worker_post_show_cli(msg) != 0)
                {
                    LOG_WARN("BGP: Failed to forward CLI show command to worker thread");
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                bgp_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }
        case CLI_MSG_TYPE_SHOW_CONFIG:
        {
            bgp_bdr_show_config(msg);
            return;
        }
        case CLI_MSG_TYPE_CONTINUE:
        {
            if (bgp_worker_post_show_cli(msg) != 0)
            {
                LOG_WARN("BGP: Failed to forward CLI continue command to worker thread");
                dev_ipc_message_free(msg);
            }
            return;
        }
        case ROUTE_MSG_TYPE_UPDATE:
        case ROUTE_MSG_TYPE_REPORT:
        case ROUTE_MSG_TYPE_NH_NOTIFY:
        {
            if (bgp_worker_post_route_message(msg) != 0)
            {
                LOG_WARN("BGP: Failed to forward route message to worker thread (type=0x%08X)", msg->msg_type);
                dev_ipc_message_free(msg);
            }
            return;
        }
        default:
            break;
    }
}

int bgp_module_init(void)
{
    LOG_INFO("Module initialization");

    bgp_parse_init();
    bgp_pkt_build_init();

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_BGP, "bgp", DEV_MODULE_PORT_BGP, bgp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_bgp_local = g_malloc0(sizeof(bgp_local_t));
    if (!g_bgp_local)
    {
        LOG_ERROR("BGP: failed to allocate local context");
        return -1;
    }

    g_bgp_local->dev_ipc_ctx = ctx;
    g_bgp_local->epoll_fd = DEV_INVALID_FD;
    g_bgp_local->listen_fd = -1;
    g_bgp_local->cmd_eventfd = -1;
    g_bgp_local->cmd_queue = NULL;
    g_bgp_local->running = 0;
    g_bgp_local->worker_thread = 0;

    return 0;
}
