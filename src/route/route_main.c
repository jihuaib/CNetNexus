/**
 * @file   route_main.c
 * @brief  Route 模块主入口，三阶段初始化和 IPC 消息分发
 * @author jhb
 * @date   2026/02/01
 */
#include "route_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "fib.h"
#include "if.h"
#include "log.h"
#include "route.h"
#include "route_bdr.h"
#include "route_cli.h"
#include "route_db.h"
#include "route_worker.h"
#include "vrf.h"

route_local_t *g_route_local = NULL;

/* route_static 表：用户手动配置的静态路由 */
static const db_column_def_t ROUTE_STATIC_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},     {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"prefix", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},       {"prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"nexthop", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},      {"metric", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"preference", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"}, {"ifname", DB_TYPE_TEXT, 0, ""},
};

static const db_table_def_t ROUTE_STATIC_TABLE = {
    .table_name = "route_static",
    .cols = ROUTE_STATIC_COLS,
    .num_cols = G_N_ELEMENTS(ROUTE_STATIC_COLS),
};

/* route_batch 表：批量路由配置（name 为主键，存储 batch 参数用于重启恢复） */
static const db_column_def_t ROUTE_BATCH_COLS[] = {
    {"name", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY | DB_COL_NOT_NULL, NULL},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"start_addr", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"count", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"nexthop", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
};

static const db_table_def_t ROUTE_BATCH_TABLE = {
    .table_name = "route_batch",
    .cols = ROUTE_BATCH_COLS,
    .num_cols = G_N_ELEMENTS(ROUTE_BATCH_COLS),
};

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_ROUTE,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START
// ============================================================================

static void route_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB) < 0)
    {
        LOG_ERROR("Failed to connect to DB module");
    }

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI) != 0)
    {
        LOG_ERROR("Failed to connect to CLI module");
    }

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_IF, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_IF) != 0)
    {
        LOG_WARN("Failed to connect to IF module (interface names will show as ifindex)");
    }

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_FIB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_FIB) != 0)
    {
        LOG_WARN("Failed to connect to FIB module (best routes will not be programmed)");
    }
    if (dev_ipc_connect(ctx, DEV_MODULE_ID_VRF, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_VRF) != 0)
    {
        LOG_WARN("Failed to connect to VRF module (VRF-name show filters may be unavailable)");
    }

    /*
     * 线程化后 ROUTE_MSG_TYPE_INJECT/NH_* 可能在 MODULE_READY 前到达（例如 IF 在其 READY 阶段恢复直连路由）。
     * worker 必须在 START 阶段就绪，避免早期业务消息因 cmd_queue 未创建被丢弃。
     */
    if (route_worker_prepare() < 0)
    {
        LOG_ERROR("Route worker prepare failed");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    if (route_worker_launch() < 0)
    {
        LOG_ERROR("Route worker launch failed");
        route_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    LOG_INFO("Connected to DB, CLI, IF");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT
// ============================================================================

static void route_on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY
// ============================================================================

static void route_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Initializing Route database");

    int ret = db_rpc_create_table_from_def(ctx, &ROUTE_STATIC_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route table creation failed: route_static");
        send_phase_response(ctx, msg, ERRCODE_SUCCESS);
        return;
    }

    ret = db_rpc_create_table_from_def(ctx, &ROUTE_BATCH_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route table creation failed: route_batch");
    }

    if (route_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Route DB restore failed");
        route_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    /* 通过 if_api 订阅 IF 全量事件，用于维护统一接口缓存 */
    if (if_api_subscribe_all(ctx) == ERRCODE_SUCCESS)
    {
        LOG_INFO("Subscribed to IF events via if_api (ALL types, ALL events)");
    }
    else
    {
        LOG_WARN("Failed to subscribe to IF events via if_api");
    }
    if (vrf_api_subscribe_all(ctx) == ERRCODE_SUCCESS)
    {
        LOG_INFO("Subscribed to VRF events via vrf_api");
    }
    else
    {
        LOG_WARN("Failed to subscribe to VRF events via vrf_api");
    }

    LOG_INFO("Route database tables ready");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

/**
 * @brief 读取 CLI 消息 payload 中的 flags 字节
 */
static uint8_t route_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

void route_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    if (msg == NULL)
    {
        return;
    }

    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            route_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            route_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            route_on_ready(msg);
            return;

        /* ---- CLI 命令 ---- */
        case CLI_MSG_TYPE:
        {
            uint8_t flags = route_cli_payload_flags(msg);
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                /* show 命令：异步转发 worker 线程处理（访问内存 RIB） */
                LOG_DEBUG("Received CLI show command, forwarding to worker");
                if (route_worker_post_show_cli(msg) != 0)
                {
                    LOG_WARN("Route: Failed to forward CLI show command to worker thread");
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                /* 配置命令：在 IPC 线程完成 DB 持久化后同步等待 worker 完成内存应用 */
                LOG_DEBUG("Received CLI config command (%u bytes)", msg->payload_len);
                route_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }

        case CLI_MSG_TYPE_CONTINUE:
            /* 分片继续请求：转发 worker 线程（show 流状态在 worker） */
            LOG_DEBUG("Received CLI continue request");
            if (route_worker_post_show_cli(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            /* show current-configuration：在 IPC 线程直接处理（BDR 读 DB 并批量输出） */
            LOG_DEBUG("Received show current-configuration request");
            route_bdr_handle_show_config(msg);
            dev_ipc_message_free(msg);
            return;

        /* ---- 路由业务消息 ---- */
        case ROUTE_MSG_TYPE_SUBSCRIBE:
            if (route_worker_post(ROUTE_WORKER_CMD_SUBSCRIBE, msg) != 0)
            {
                LOG_WARN("Route: failed to post SUBSCRIBE to worker");
                dev_ipc_message_free(msg);
            }
            return;

        case ROUTE_MSG_TYPE_UNSUBSCRIBE:
            if (route_worker_post(ROUTE_WORKER_CMD_UNSUBSCRIBE, msg) != 0)
            {
                LOG_WARN("Route: failed to post UNSUBSCRIBE to worker");
                dev_ipc_message_free(msg);
            }
            return;

        case ROUTE_MSG_TYPE_INJECT:
            if (route_worker_post(ROUTE_WORKER_CMD_INJECT, msg) != 0)
            {
                LOG_WARN("Route: failed to post INJECT to worker");
                if (msg->request_id != 0)
                {
                    route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
                    if (ack)
                    {
                        ack->result = ERRCODE_FAIL;
                        dev_ipc_message_t *resp =
                            dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                   msg->request_id, ack, sizeof(route_msg_ack_t), g_free);
                        if (resp)
                        {
                            dev_ipc_send_response(route_local_ipc_ctx(), resp);
                            dev_ipc_message_free(resp);
                        }
                        else
                        {
                            g_free(ack);
                        }
                    }
                }
                dev_ipc_message_free(msg);
            }
            return;
        case ROUTE_MSG_TYPE_NH_REGISTER:
            if (route_worker_post(ROUTE_WORKER_CMD_NH_REGISTER, msg) != 0)
            {
                LOG_WARN("Route: failed to post NH_REGISTER to worker");
                dev_ipc_message_free(msg);
            }
            return;
        case ROUTE_MSG_TYPE_NH_UNREGISTER:
            if (route_worker_post(ROUTE_WORKER_CMD_NH_UNREGISTER, msg) != 0)
            {
                LOG_WARN("Route: failed to post NH_UNREGISTER to worker");
                dev_ipc_message_free(msg);
            }
            return;

        /* ---- IF 事件通知 ---- */
        case IF_MSG_TYPE_EVENT:
            if (route_worker_post(ROUTE_WORKER_CMD_IF_EVENT, msg) != 0)
            {
                LOG_WARN("Route: failed to post IF_EVENT to worker");
                dev_ipc_message_free(msg);
            }
            return;

        case IF_MSG_TYPE_ACK:
            /* IF 订阅应答，静默丢弃 */
            break;

        case VRF_MSG_TYPE_EVENT:
            if (route_worker_dispatch_vrf_event(msg) != 0)
            {
                LOG_WARN("Route: failed to dispatch VRF_EVENT to worker");
                dev_ipc_message_free(msg);
            }
            return;

        case VRF_MSG_TYPE_ACK:
            /* VRF 订阅应答，静默丢弃 */
            break;

        case FIB_MSG_TYPE_ROUTE_RESULT:
            if (route_worker_post(ROUTE_WORKER_CMD_FIB_ROUTE_RESULT, msg) != 0)
            {
                LOG_WARN("Route: failed to post FIB_ROUTE_RESULT");
                dev_ipc_message_free(msg);
            }
            return;

        default:
            LOG_WARN("Received unknown message type: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// Module initialization
// ============================================================================

int route_module_init(void)
{
    log_set_tag("route");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_ROUTE, "route", DEV_MODULE_PORT_ROUTE, route_ipc_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_route_local = (route_local_t *)g_malloc0(sizeof(route_local_t));
    if (!g_route_local)
    {
        LOG_ERROR("Failed to allocate route context");
        return -1;
    }

    g_route_local->dev_ipc_ctx = ctx;

    return 0;
}

void route_module_cleanup(void)
{
    dev_ipc_context_t *ctx = NULL;
    if (g_route_local)
    {
        ctx = g_route_local->dev_ipc_ctx;
        g_route_local->dev_ipc_ctx = NULL;
    }

    /* 再停止 route worker，触发 route_calc_cleanup 撤销 FIB 路由。 */
    route_worker_shutdown();

    /* 先停止 IPC 线程，避免退出过程中继续接收业务消息。 */
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    if (!g_route_local)
    {
        return;
    }

    if (g_route_local)
    {
        g_free(g_route_local);
        g_route_local = NULL;
    }
}
