/**
 * @file   route_main.c
 * @brief  Route 模块主入口，三阶段初始化和 IPC 消息分发
 * @author jhb
 * @date   2026/02/01
 */
#include "route_main.h"

#include <stddef.h>
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
#include "pending.h"
#include "route.h"
#include "route_bdr.h"
#include "route_cfg_apply.h"
#include "route_cli.h"
#include "route_db.h"
#include "route_worker.h"
#include "vrf.h"

route_local_t *g_route_local = NULL;

/* route_static 表：用户手动配置的静态路由（vrf_name 持久化；vrf_id 仅在内存有效，重启可变） */
static const db_column_def_t ROUTE_STATIC_COLS[] = {
    {"vrf_name", DB_TYPE_TEXT, DB_COL_NOT_NULL, VRF_PUBLIC_VRF_NAME},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"prefix", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"nexthop", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"metric", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"preference", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"ifname", DB_TYPE_TEXT, 0, ""},
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

// ============================================================================
// Phase 1: MODULE_START
// ============================================================================

/* VRF dep 就绪回调（含初次 + 重启）。
 * IO/同步上下文，不阻塞——投递 worker 内部消息让 worker 线程做实际订阅。 */
static void route_on_vrf_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
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
    if (!g_route_local || !g_route_local->dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(ROUTE_MSG_TYPE_INTERNAL_VRF_READY, DEV_MODULE_ID_ROUTE,
                                                  DEV_MODULE_ID_ROUTE, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_route_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void route_handle_vrf_ready(void)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_VRF, 3000) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: VRF not connected within 3s; vrf_api_subscribe deferred");
        return;
    }
    if (vrf_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: vrf_api_subscribe_all failed");
    }
    else
    {
        LOG_INFO("Route: subscribed to VRF events");
    }
}

static void route_handle_if_ready(void)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, 3000) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: IF not connected within 3s; if_api_subscribe_all deferred");
        return;
    }
    if (if_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: if_api_subscribe_all failed");
    }
    else
    {
        LOG_INFO("Route: subscribed to IF events");
    }
}

/* IF dep 事件回调：
 *   READY → 投递 IF_READY，IPC 线程做 if_api_subscribe_all（支持 IF 重启）。
 *   DOWN  → 投递 IF_DOWN，worker 清 IF 缓存 + 重算 nexthop watch + 通知 BGP 等订阅方。 */
static void route_on_if_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                 void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (!g_route_local || !g_route_local->dev_ipc_ctx)
    {
        return;
    }
    uint32_t msg_type;
    if (event == DEV_MODULE_EVENT_READY)
    {
        msg_type = ROUTE_MSG_TYPE_INTERNAL_IF_READY;
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        msg_type = ROUTE_MSG_TYPE_INTERNAL_IF_DOWN;
    }
    else
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(msg_type, DEV_MODULE_ID_ROUTE, DEV_MODULE_ID_ROUTE, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_route_local->dev_ipc_ctx->msg_queue, m);
    }
}

/**
 * Route 本地 init（合并原 on_start + on_ready 逻辑）。
 * 顺序：worker 起来 → 订阅依赖 → 建表 restore → 订阅 CLI → notify_ready
 */
static int route_init_local(void)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, 10000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Route: timed out waiting for DEV connection");
    }

    /* worker 必须先就绪：ROUTE_MSG_TYPE_INJECT/NH_* 等业务消息可能很早到达 */
    if (route_worker_prepare() < 0 || route_worker_launch() < 0)
    {
        LOG_ERROR("Route: worker start failed");
        route_worker_shutdown();
        return -1;
    }

    /* DB 等基础模块只 kick 连接，worker 后续 RPC 时已就绪 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(DB) failed");
    }
    /* IF：注册 cb 感知 READY/DOWN；事件订阅由 if_api_subscribe_all 发起 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0, route_on_if_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(IF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_FIB, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(FIB) failed");
    }
    /* VRF（on-demand）：auto_start=1 触发 + cb 重启感知 */
    /* VRF 用 auto_start=0：ROUTE 不硬依赖 VRF；cb 在 VRF 实际启动后才触发 vrf_api_subscribe_all */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_VRF, 0, route_on_vrf_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(VRF) failed");
    }

    /* DB 建表（同步 RPC，DB 必须可达；DB 是基础模块，wait_connected 通常瞬间完成） */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, 5000) == ERRCODE_SUCCESS)
    {
        if (db_rpc_create_table_from_def(ctx, &ROUTE_STATIC_TABLE) != ERRCODE_SUCCESS)
        {
            LOG_WARN("Route: create table route_static failed");
        }
        if (db_rpc_create_table_from_def(ctx, &ROUTE_BATCH_TABLE) != ERRCODE_SUCCESS)
        {
            LOG_WARN("Route: create table route_batch failed");
        }
        if (route_db_restore() != ERRCODE_SUCCESS)
        {
            LOG_WARN("Route: DB restore failed");
        }
    }
    else
    {
        LOG_WARN("Route: DB not reachable, skipped init/restore");
    }

    /* if_api_subscribe_all 是 send → 需要 IF 连接已建立；basic 模块 IF 一般已 up */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, 3000) == ERRCODE_SUCCESS)
    {
        if (if_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
        {
            LOG_WARN("Route: if_api_subscribe_all failed");
        }
    }

    /* subscribe(CLI) 末尾，CFG 看到 is_connected 即本模块已 fully ready */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(CLI) failed");
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: notify_ready to DEV failed");
    }
    LOG_INFO("Route: module ready");
    return 0;
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
    if (msg == NULL)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case ROUTE_MSG_TYPE_INTERNAL_VRF_READY:
            route_handle_vrf_ready();
            break;

        case ROUTE_MSG_TYPE_INTERNAL_IF_READY:
            route_handle_if_ready();
            break;

        case ROUTE_MSG_TYPE_INTERNAL_IF_DOWN:
            if (route_worker_post(ROUTE_WORKER_CMD_IF_DOWN, NULL) != 0)
            {
                LOG_WARN("Route: failed to post IF-down recompute to worker");
            }
            break;

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
                /* DB 不可用时直接拒绝配置下发，避免内存/OS 与 DB 静默偏移 */
                if (db_rpc_guard_reject(ctx, msg, "Route"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
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
        {
            /* 提取 VRF 事件类型与名称（worker dispatch 后 msg 已被消费，不可再访问） */
            uint32_t vrf_event = 0;
            uint32_t vrf_id = 0;
            char vrf_name[VRF_NAME_MAX_LEN] = {0};
            if (msg->payload && msg->payload_len >= offsetof(vrf_event_msg_t, rts))
            {
                const vrf_event_msg_t *evt = (const vrf_event_msg_t *)msg->payload;
                vrf_event = evt->event;
                vrf_id = evt->vrf_id;
                g_strlcpy(vrf_name, evt->name, sizeof(vrf_name));
            }

            if (route_worker_dispatch_vrf_event(msg) != 0)
            {
                LOG_WARN("Route: failed to dispatch VRF_EVENT to worker");
                dev_ipc_message_free(msg);
                return;
            }

            if (vrf_name[0] != '\0')
            {
                if (vrf_event == VRF_EVENT_VRF_ADD)
                {
                    pending_resolve(g_route_local->pending, ROUTE_DEP_VRF, g_str_hash(vrf_name));
                }
                else if (vrf_event == VRF_EVENT_VRF_DEL)
                {
                    pending_invalidate(g_route_local->pending, ROUTE_DEP_VRF, g_str_hash(vrf_name));

                    /* 级联：删除该 VRF 下所有静态路由（DB + 内存 RIB） */
                    if (vrf_id != ROUTE_VRF_DEFAULT)
                    {
                        route_apply_cmd_t apply;
                        memset(&apply, 0, sizeof(apply));
                        apply.op = ROUTE_APPLY_STATIC_DEL_VRF;
                        apply.u.static_del_vrf.vrf_id = vrf_id;
                        route_worker_dispatch_apply(&apply);

                        int db_rc = route_db_delete_static_by_vrf(route_local_ipc_ctx(), vrf_name);
                        LOG_INFO("Route: VRF '%s' (id=%u) deleted, cascaded %d static path(s), db rc=%d", vrf_name,
                                 vrf_id, apply.rc > 0 ? apply.rc : 0, db_rc);
                    }
                }
            }
            return;
        }

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
    g_route_local->pending = pending_new("route");

    return route_init_local();
}

void route_module_cleanup(void)
{
    dev_ipc_context_t *ctx = NULL;
    if (g_route_local)
    {
        ctx = g_route_local->dev_ipc_ctx;
    }

    /* 先停止 route worker，保留 IPC ctx 供 route_calc_cleanup 撤销 FIB 路由。 */
    route_worker_shutdown();

    if (g_route_local)
    {
        g_route_local->dev_ipc_ctx = NULL;
    }

    /* 再停止 IPC 线程，避免退出过程中继续接收业务消息。 */
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    if (!g_route_local)
    {
        return;
    }

    if (g_route_local->pending)
    {
        pending_destroy(g_route_local->pending);
        g_route_local->pending = NULL;
    }

    g_free(g_route_local);
    g_route_local = NULL;
}
