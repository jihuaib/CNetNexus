/**
 * @file   ldp_main.c
 * @brief  LDP 模块主入口：生命周期与 IPC 消息分发
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_main.h"

#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "if.h"
#include "ldp_bdr.h"
#include "ldp_cli.h"
#include "ldp_db.h"
#include "log.h"
#include "route.h"
#include "work/ldp_worker.h"

ldp_local_t *g_ldp_local = NULL;

static uint8_t ldp_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

/* ============================================================================
 * IF dep 就绪回调（在 IF 每次 READY 时触发，含初次 + 重启）
 *
 * cb 在 IO/订阅响应上下文调用，不能阻塞——只投递自身内部消息让 worker 线程处理。
 * Worker 线程做 wait_connected(IF, ...) + if_api_subscribe_all，保证 IF 重启后自动重新订阅。
 * ============================================================================ */

/* IF dep 事件回调：
 *   READY → 投递 IF_READY，worker 做 if_api_subscribe_all 重新订阅（支持重启）。
 *   DOWN  → 投递 IF_DOWN，worker 立刻关接口 socket + 拆所有 hello/会话，
 *           避免靠 keepalive 超时被动感知。 */
static void ldp_on_if_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                               void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (!g_ldp_local || !g_ldp_local->dev_ipc_ctx)
    {
        return;
    }
    uint32_t msg_type;
    if (event == DEV_MODULE_EVENT_READY)
    {
        msg_type = LDP_MSG_TYPE_INTERNAL_IF_READY;
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        msg_type = LDP_MSG_TYPE_INTERNAL_IF_DOWN;
    }
    else
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(msg_type, DEV_MODULE_ID_LDP, DEV_MODULE_ID_LDP, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_ldp_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void ldp_on_route_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
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
    if (!g_ldp_local || !g_ldp_local->dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(LDP_MSG_TYPE_INTERNAL_ROUTE_READY, DEV_MODULE_ID_LDP,
                                                  DEV_MODULE_ID_LDP, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_ldp_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void ldp_handle_if_ready(void)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    /* 等 IPC 库异步握手完成，再下 subscribe 请求 */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, 3000) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: IF connection not ready after 3s; subscribe deferred to next READY");
        return;
    }
    if (if_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: if_api_subscribe_all failed");
    }
    else
    {
        LOG_INFO("LDP: subscribed to IF events");
    }
}

/**
 * 等 DB 就绪后建表 + restore。
 */
static int ldp_init_db_state(void)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();

    if (dev_ipc_wait_module_ready(ctx, DEV_MODULE_ID_DB, 5000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: DB not ready, skip db init/restore");
        return -1;
    }
    if (ldp_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: DB init failed");
        return -1;
    }
    if (ldp_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: DB restore failed");
        /* 不致命：可能 db_init 成功但表里数据有问题；继续启动让用户排查 */
    }
    return 0;
}

void ldp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case LDP_MSG_TYPE_INTERNAL_IF_READY:
            ldp_handle_if_ready();
            break;

        case LDP_MSG_TYPE_INTERNAL_IF_DOWN:
            if (ldp_worker_post_if_down() != ERRCODE_SUCCESS)
            {
                LOG_WARN("LDP: failed to post IF-down teardown");
            }
            break;

        case LDP_MSG_TYPE_INTERNAL_ROUTE_READY:
            if (ldp_worker_post_route_ready() != ERRCODE_SUCCESS)
            {
                LOG_WARN("LDP: failed to post ROUTE-ready resubscribe");
            }
            break;

        case CLI_MSG_TYPE:
        {
            uint8_t flags = ldp_cli_payload_flags(msg);
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (ldp_worker_post_show_cli(msg) != 0)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                /* DB 不可用时直接拒绝配置下发，避免内存/OS 与 DB 静默偏移 */
                if (db_rpc_guard_reject(ctx, msg, "LDP"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
                (void)ldp_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }

        case CLI_MSG_TYPE_CONTINUE:
            if (ldp_worker_post_show_cli(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)ldp_bdr_handle_show_config(msg);
            dev_ipc_message_free(msg);
            return;

        case IF_MSG_TYPE_EVENT:
            if (ldp_worker_post_if_event(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

        case ROUTE_MSG_TYPE_REPORT:
        case ROUTE_MSG_TYPE_UPDATE:
            if (ldp_worker_post_route_msg(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

int ldp_module_init(void)
{
    log_set_tag("ldp");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_LDP, "ldp", DEV_MODULE_PORT_LDP, ldp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("LDP: IPC initialization failed");
        return -1;
    }

    g_ldp_local = g_malloc0(sizeof(ldp_local_t));
    if (!g_ldp_local)
    {
        LOG_ERROR("LDP: failed to allocate local context");
        dev_ipc_destroy(ctx);
        return -1;
    }

    g_ldp_local->dev_ipc_ctx = ctx;

    /* 弱依赖模型启动：
     *   1. 等 DEV 控制连接
     *   2. 业务模块依赖声明：subscribe(TUNNEL, auto_start=1) —— LDP 需要 TUNNEL 做标签转发
     *   3. wait_module_ready(DB) → db_init + db_restore
     *   4. worker 启动 + 订阅 IF 事件
     *   5. subscribe(CLI) 放最后：CFG 看到本模块在跑即可立即 dispatch
     *   6. notify_ready 通知 DEV */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, 10000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: timed out waiting for DEV connection; module may be unusable");
    }

    /* 显式依赖：使能 LDP 即拉起 TUNNEL（按需模块，否则保持 idle）。
     * 不带回调——纯触发，IPC 库自动建联，运行时 RPC 走该连接。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_TUNNEL, 1, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: subscribe(TUNNEL) failed; MPLS forwarding may not work");
    }

    (void)ldp_init_db_state();

    if (ldp_worker_prepare() != ERRCODE_SUCCESS || ldp_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: worker start failed");
        ldp_worker_shutdown();
        return -1;
    }

    /* ROUTE：回调模式，ROUTE 每次 READY 触发 worker 重发路由订阅。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0, ldp_on_route_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: subscribe(ROUTE) failed");
    }

    /* IF：用回调模式。on_if_ready 在 IF 每次 READY 时（含首次/重启）触发，
     * 投递 worker 内部消息，由 worker 线程做实际的 if_api_subscribe_all。
     * 这样 init 完全不阻塞 IF 的可达性，IF 重启也能自动重新订阅事件。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0, ldp_on_if_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: subscribe(IF) failed");
    }

    /* 放在末尾：CFG poll is_connected(LDP)=true 时本模块已 fully ready */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: subscribe(CLI) failed; commands from CFG won't be reachable");
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: notify_ready to DEV failed");
    }
    LOG_INFO("LDP: module ready");

    return 0;
}

void ldp_module_cleanup(void)
{
    if (!g_ldp_local)
    {
        return;
    }

    ldp_worker_shutdown();

    dev_ipc_context_t *ctx = g_ldp_local->dev_ipc_ctx;
    g_ldp_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_ldp_local);
    g_ldp_local = NULL;
}
