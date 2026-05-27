/**
 * @file   isis_main.c
 * @brief  ISIS 模块主入口：生命周期处理与消息分发
 * @author jhb
 * @date   2026/04/11
 */
#include "isis_main.h"

#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "if.h"
#include "isis_bdr.h"
#include "isis_cli.h"
#include "isis_db.h"
#include "isis_worker.h"
#include "log.h"

isis_local_t *g_isis_local = NULL;

static uint8_t isis_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

/* IF dep 事件回调：
 *   READY → 投递 IF_READY，worker 做 if_api_subscribe_all 重新订阅（支持重启）。
 *   DOWN  → 投递 IF_DOWN，worker 立刻清 IF 缓存 + 拆所有邻接 + 撤销 ISIS 路由，
 *           不等 hello hold-time（~9s）自然失效。 */
static void isis_on_if_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (!g_isis_local || !g_isis_local->dev_ipc_ctx)
    {
        return;
    }
    uint32_t msg_type;
    if (event == DEV_MODULE_EVENT_READY)
    {
        msg_type = ISIS_MSG_TYPE_INTERNAL_IF_READY;
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        msg_type = ISIS_MSG_TYPE_INTERNAL_IF_DOWN;
    }
    else
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(msg_type, DEV_MODULE_ID_ISIS, DEV_MODULE_ID_ISIS, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_isis_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void isis_on_route_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
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
    if (!g_isis_local || !g_isis_local->dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(ISIS_MSG_TYPE_INTERNAL_ROUTE_READY, DEV_MODULE_ID_ISIS,
                                                  DEV_MODULE_ID_ISIS, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_isis_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void isis_handle_if_ready(void)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: IF connection not ready in time; subscribe deferred to next READY");
        return;
    }
    if (if_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: if_api_subscribe_all failed");
    }
    else
    {
        LOG_INFO("ISIS: subscribed to IF events");
    }
}

/**
 * 等 DB 就绪后建表。配置 restore 需要 worker 已启动，因为 restore 通过 apply
 * 命令回放到 worker 状态。
 */
static gboolean g_isis_db_restored = FALSE;
static gboolean g_isis_db_ready = FALSE;     /* DB 已建表 */
static gboolean g_isis_if_smoothend = FALSE; /* IF REPLAY 已完成 */

static void isis_try_db_restore(void)
{
    if (g_isis_db_restored)
    {
        return;
    }
    if (!g_isis_db_ready || !g_isis_if_smoothend)
    {
        return;
    }
    if (isis_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: DB restore failed");
        return;
    }
    g_isis_db_restored = TRUE;
    LOG_INFO("ISIS: DB restore completed");
}

static void isis_handle_db_ready(void)
{
    /* DB MODULE_EVENT READY 触发：等握手完成（subscribe / event 只是触发 connect，IO 线程异步建联）。
     * db_init 幂等。 */
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: DB not connected in time; db restore deferred");
        return;
    }

    if (isis_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: DB init failed");
        return;
    }
    g_isis_db_ready = TRUE;
    isis_try_db_restore();
}

static void isis_handle_if_smoothend(void)
{
    gboolean first = !g_isis_if_smoothend;
    g_isis_if_smoothend = TRUE;
    if (first)
    {
        LOG_INFO("ISIS: IF smoothend received (initial sync)");
        isis_try_db_restore();
    }
    else
    {
        LOG_INFO("ISIS: IF smoothend received (resync)");
    }
}

static void isis_on_db_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event != DEV_MODULE_EVENT_READY || !g_isis_local || !g_isis_local->dev_ipc_ctx)
    {
        return;
    }

    dev_ipc_message_t *m = dev_ipc_message_create(ISIS_MSG_TYPE_INTERNAL_DB_READY, DEV_MODULE_ID_ISIS,
                                                  DEV_MODULE_ID_ISIS, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_isis_local->dev_ipc_ctx->msg_queue, m);
    }
}

void isis_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    /* cleanup 阶段:worker 已经/正在销毁,直接丢弃避免 worker_post_* 撞 NULL g_isis_work_local */
    if (g_isis_local && g_isis_local->shutting_down)
    {
        dev_ipc_message_free(msg);
        return;
    }

    switch (msg->msg_type)
    {
        case ISIS_MSG_TYPE_INTERNAL_DB_READY:
            isis_handle_db_ready();
            break;

        case ISIS_MSG_TYPE_INTERNAL_IF_READY:
            isis_handle_if_ready();
            break;

        case ISIS_MSG_TYPE_INTERNAL_ROUTE_READY:
            if (isis_worker_post_route_ready() != ERRCODE_SUCCESS)
            {
                LOG_WARN("ISIS: failed to post ROUTE-ready replay");
            }
            break;

        case ISIS_MSG_TYPE_INTERNAL_IF_DOWN:
            if (isis_worker_post_if_down() != ERRCODE_SUCCESS)
            {
                LOG_WARN("ISIS: failed to post IF-down teardown");
            }
            break;

        case CLI_MSG_TYPE:
        {
            uint8_t flags = isis_cli_payload_flags(msg);
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (isis_worker_post_show_cli(msg) != 0)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                /* DB 不在线时拒绝配置：避免内存改了 / DB 写不到的静默偏移 */
                if (db_rpc_guard_reject(ctx, msg, "ISIS"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
                (void)isis_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }

        case CLI_MSG_TYPE_CONTINUE:
            if (isis_worker_post_show_cli(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)isis_bdr_handle_show_config(msg);
            dev_ipc_message_free(msg);
            return;

        case IF_MSG_TYPE_EVENT:
        {
            uint32_t if_event = 0;
            if (msg->payload && msg->payload_len >= sizeof(if_event_msg_t))
            {
                if_event = ((const if_event_msg_t *)msg->payload)->event;
            }
            if (if_event == IF_EVENT_SMOOTHEND)
            {
                isis_handle_if_smoothend();
            }
            if (isis_worker_post_if_event(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;
        }

        case IF_MSG_TYPE_ACK:
            break;

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

int isis_module_init(void)
{
    log_set_tag("isis");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_ISIS, "isis", DEV_MODULE_PORT_ISIS, isis_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("ISIS: IPC initialization failed");
        return -1;
    }

    g_isis_local = g_malloc0(sizeof(isis_local_t));
    if (!g_isis_local)
    {
        LOG_ERROR("ISIS: failed to allocate local context");
        dev_ipc_destroy(ctx);
        return -1;
    }

    g_isis_local->dev_ipc_ctx = ctx;

    /* 弱依赖模型启动：
     *   1. 等 DEV 控制连接
     *   2. worker 启动
     *   3. 订阅所有依赖（含 CLI）
     *   4. wait_all_subscribed_connected：等所有 peer IPC 都 CONNECTED
     *   5. db_init + db_restore
     *   6. notify_ready：业务真正可用 */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: timed out waiting for DEV connection; module may be unusable");
    }

    if (isis_worker_prepare() != ERRCODE_SUCCESS || isis_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: worker start failed");
        isis_worker_shutdown();
        return -1;
    }

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0, isis_on_route_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: subscribe(ROUTE) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0, isis_on_if_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: subscribe(IF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, isis_on_db_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: subscribe(DB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: subscribe(CLI) failed");
    }

    /* 阻塞直到所有订阅 peer IPC 都 CONNECTED 再 notify_ready，避免 CFG 端发命令 race。 */
    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0);

    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        isis_handle_db_ready();
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: notify_ready to DEV failed");
    }

    LOG_INFO("ISIS: module ready");

    return 0;
}

void isis_module_cleanup(void)
{
    if (!g_isis_local)
    {
        return;
    }

    /* 1) 置 shutting_down,新到达 msg_handler 一律丢弃。
     *    这样 worker_shutdown 期间不会有新的 worker_post_* 进入异步队列。 */
    g_isis_local->shutting_down = 1;

    /* 2) worker 仍在跑、IPC 仍可用 → withdraw_all_instance_routes 才能真正发出 RPC 到 ROUTE,
     *    清掉 RIB 中的 ISIS 条目。原顺序在此之前就 dev_ipc_destroy 了,导致撤销静默失败。 */
    isis_worker_shutdown();

    /* 3) 关 IPC,join IO/worker 线程,断所有连接。 */
    dev_ipc_context_t *ctx = g_isis_local->dev_ipc_ctx;
    g_isis_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_isis_local);
    g_isis_local = NULL;
}
