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
#include "route.h"
#include "route_bdr.h"
#include "route_cfg_apply.h"
#include "route_cli.h"
#include "route_db.h"
#include "route_worker.h"
#include "vrf.h"

route_local_t *g_route_local = NULL;
static gboolean g_route_db_restored = FALSE;

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

/* VRF dep 事件回调（含初次 + 重启）。
 *   READY → 投递 VRF_READY，IPC 线程做 vrf_api_subscribe_all。
 *   DOWN  → 投递 VRF_DOWN，worker 拆非 public VRF 业务 + 清 cache。 */
static void route_on_vrf_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
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
        msg_type = ROUTE_MSG_TYPE_INTERNAL_VRF_READY;
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        msg_type = ROUTE_MSG_TYPE_INTERNAL_VRF_DOWN;
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

static void route_handle_vrf_ready(void)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_VRF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: VRF not connected in time; vrf_api_subscribe deferred");
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
    /* IF READY 事件可能比 IF 连接建立先到（synth READY 来自 DEV，连接由 IO 异步建）。
     * 用 wait_connected 给点时间，避免一次性 deferred 后再没人补订阅。 */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: IF not connected in time; if_api_subscribe_all deferred");
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

static gboolean g_route_vrf_smoothend = FALSE; /* VRF REPLAY 已完成 */
static gboolean g_route_if_smoothend = FALSE;  /* IF REPLAY 已完成 */

static void route_try_db_restore(void)
{
    if (g_route_db_restored)
    {
        return;
    }
    if (!g_route_vrf_smoothend || !g_route_if_smoothend)
    {
        return;
    }

    if (route_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: DB restore failed");
        return;
    }
    g_route_db_restored = TRUE;
    LOG_INFO("Route: DB restore completed");
}

static void route_handle_db_ready(void)
{
    /* DB MODULE_EVENT READY 触发：等握手完成（subscribe / event 只是触发 connect，IO 线程异步建联）。
     * CREATE TABLE IF NOT EXISTS 幂等。 */
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: DB not connected in time; db restore deferred");
        return;
    }

    if (db_rpc_create_table_from_def(ctx, &ROUTE_STATIC_TABLE) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: create table route_static failed");
        return;
    }
    if (db_rpc_create_table_from_def(ctx, &ROUTE_BATCH_TABLE) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: create table route_batch failed");
        return;
    }

    route_try_db_restore();
}

static void route_handle_vrf_smoothend(void)
{
    gboolean first = !g_route_vrf_smoothend;
    g_route_vrf_smoothend = TRUE;

    if (first)
    {
        LOG_INFO("Route: VRF smoothend received (initial sync)");
        route_try_db_restore();
        return;
    }

    /* VRF 进程重启后的再同步：worker 已在 SMOOTHSTART 时清掉非 public VRF 的内存静态路由，
     * 这里只从 DB 重恢复 vrf_name 非 public 的行。 */
    LOG_INFO("Route: VRF smoothend received (resync)");
    (void)route_db_restore_vrf_bound();
}

static void route_handle_if_smoothend(void)
{
    gboolean first = !g_route_if_smoothend;
    g_route_if_smoothend = TRUE;

    if (first)
    {
        LOG_INFO("Route: IF smoothend received (initial sync)");
        route_try_db_restore();
    }
    else
    {
        LOG_INFO("Route: IF smoothend received (resync)");
    }
}

static void route_on_db_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                 void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event != DEV_MODULE_EVENT_READY || !g_route_local || !g_route_local->dev_ipc_ctx)
    {
        return;
    }

    dev_ipc_message_t *m = dev_ipc_message_create(ROUTE_MSG_TYPE_INTERNAL_DB_READY, DEV_MODULE_ID_ROUTE,
                                                  DEV_MODULE_ID_ROUTE, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_route_local->dev_ipc_ctx->msg_queue, m);
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
 * 顺序：worker 起来 → notify_ready → 订阅依赖；DB/IF READY 后再做各自恢复/事件订阅
 */
static int route_init_local(void)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
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

    /* 一次性订阅所有依赖（含 CLI） */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0, route_on_if_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(IF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_FIB, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(FIB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_VRF, 0, route_on_vrf_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(VRF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, route_on_db_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(DB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route: subscribe(CLI) failed");
    }

    /* DEPS_READY：阻塞直到所有订阅 peer IPC 都 CONNECTED 再继续 DB 恢复 + notify_ready。
     * 不能用超时后继续 —— DEV 视角 READY 而 CFG 还连不上 ROUTE 会让命令派发踩到 race。 */
    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0);

    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        route_handle_db_ready();
    }

    /* 业务恢复完成，进入 READY 阶段 */
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

        case ROUTE_MSG_TYPE_INTERNAL_VRF_DOWN:
            if (route_worker_post(ROUTE_WORKER_CMD_VRF_DOWN, NULL) != 0)
            {
                LOG_WARN("Route: failed to post VRF-down purge to worker");
            }
            break;

        case ROUTE_MSG_TYPE_INTERNAL_DB_READY:
            route_handle_db_ready();
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
                /* DB 不在线时拒绝配置：避免内存改了 / DB 写不到的静默偏移 */
                if (db_rpc_guard_reject(ctx, msg, "Route"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
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
        {
            uint32_t if_event = 0;
            if (msg->payload && msg->payload_len >= sizeof(if_event_msg_t))
            {
                if_event = ((const if_event_msg_t *)msg->payload)->event;
            }
            if (if_event == IF_EVENT_SMOOTHEND)
            {
                route_handle_if_smoothend();
            }
            if (route_worker_post(ROUTE_WORKER_CMD_IF_EVENT, msg) != 0)
            {
                LOG_WARN("Route: failed to post IF_EVENT to worker");
                dev_ipc_message_free(msg);
            }
            return;
        }

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

            if (vrf_event == VRF_EVENT_SMOOTHEND)
            {
                route_handle_vrf_smoothend();
            }

            if (route_worker_dispatch_vrf_event(msg) != 0)
            {
                LOG_WARN("Route: failed to dispatch VRF_EVENT to worker");
                dev_ipc_message_free(msg);
                return;
            }

            if (vrf_name[0] != '\0')
            {
                if (vrf_event == VRF_EVENT_VRF_DEL)
                {
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

    /* 向 DEV 发 PRE_EXIT 通知，等 DEV 同步完成 phase/broadcast/drop 后再 ACK。
     * 必须在 dev_ipc_destroy 之前；超时/失败不阻塞退出，SIGCHLD 路径仍会兜底清理。 */
    if (ctx)
    {
        dev_ipc_pre_exit_notify(ctx, 3000);
    }

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

    g_free(g_route_local);
    g_route_local = NULL;
}
