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
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, 3000) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: IF connection not ready after 3s; subscribe deferred to next READY");
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
static int isis_init_db_schema(void)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();

    if (dev_ipc_wait_module_ready(ctx, DEV_MODULE_ID_DB, 5000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: DB not ready, skip db init");
        return -1;
    }
    if (isis_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: DB init failed");
        return -1;
    }
    return 0;
}

static void isis_restore_db_state(void)
{
    if (isis_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: DB restore failed");
    }
}

void isis_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
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
                /* DB 不可用时直接拒绝配置下发，避免内存/OS 与 DB 静默偏移 */
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
            if (isis_worker_post_if_event(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

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
     *   2. wait_module_ready(DB) → db_init
     *   3. worker 启动
     *   4. db_restore 回放配置到 worker + 订阅 IF 事件
     *   5. subscribe(CLI) 放最后：CFG 看到本模块在跑即可立即 dispatch
     *   6. notify_ready 通知 DEV
     * ISIS 没有 on-demand dep 需要触发（IF/ROUTE 都是基础模块），运行时 RPC 调用即可。 */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, 10000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: timed out waiting for DEV connection; module may be unusable");
    }

    (void)isis_init_db_schema();

    if (isis_worker_prepare() != ERRCODE_SUCCESS || isis_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: worker start failed");
        isis_worker_shutdown();
        return -1;
    }

    isis_restore_db_state();

    /* ROUTE：回调模式，ROUTE 每次 READY 触发 worker 重刷 ISIS 路由。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0, isis_on_route_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: subscribe(ROUTE) failed");
    }

    /* IF：回调模式，IF 每次 READY 触发 worker 重新订阅事件。支持 IF 重启。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0, isis_on_if_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: subscribe(IF) failed");
    }

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: subscribe(CLI) failed; commands from CFG won't be reachable");
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

    isis_worker_shutdown();

    dev_ipc_context_t *ctx = g_isis_local->dev_ipc_ctx;
    g_isis_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_isis_local);
    g_isis_local = NULL;
}
