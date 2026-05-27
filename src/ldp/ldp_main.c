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
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: IF connection not ready in time; subscribe deferred to next READY");
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

static gboolean g_ldp_db_restored = FALSE;
static gboolean g_ldp_db_ready = FALSE;     /* DB 已建表 */
static gboolean g_ldp_if_smoothend = FALSE; /* IF REPLAY 已完成 */

static void ldp_try_db_restore(void)
{
    if (g_ldp_db_restored)
    {
        return;
    }
    if (!g_ldp_db_ready || !g_ldp_if_smoothend)
    {
        return;
    }
    if (ldp_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: DB restore failed");
        return;
    }
    g_ldp_db_restored = TRUE;
    LOG_INFO("LDP: DB restore completed");
}

static void ldp_handle_db_ready(void)
{
    /* DB MODULE_EVENT READY 触发：等握手完成（subscribe / event 只是触发 connect，IO 线程异步建联）。
     * db_init 幂等。 */
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: DB not connected in time; db restore deferred");
        return;
    }

    if (ldp_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: DB init failed");
        return;
    }
    g_ldp_db_ready = TRUE;
    ldp_try_db_restore();
}

static void ldp_handle_if_smoothend(void)
{
    gboolean first = !g_ldp_if_smoothend;
    g_ldp_if_smoothend = TRUE;
    if (first)
    {
        LOG_INFO("LDP: IF smoothend received (initial sync)");
        ldp_try_db_restore();
    }
    else
    {
        LOG_INFO("LDP: IF smoothend received (resync)");
    }
}

static void ldp_on_db_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                               void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event != DEV_MODULE_EVENT_READY || !g_ldp_local || !g_ldp_local->dev_ipc_ctx)
    {
        return;
    }

    dev_ipc_message_t *m =
        dev_ipc_message_create(LDP_MSG_TYPE_INTERNAL_DB_READY, DEV_MODULE_ID_LDP, DEV_MODULE_ID_LDP, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_ldp_local->dev_ipc_ctx->msg_queue, m);
    }
}

void ldp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case LDP_MSG_TYPE_INTERNAL_DB_READY:
            ldp_handle_db_ready();
            break;

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
                /* DB 不在线时拒绝配置：避免内存改了 / DB 写不到的静默偏移 */
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
        {
            uint32_t if_event = 0;
            if (msg->payload && msg->payload_len >= sizeof(if_event_msg_t))
            {
                if_event = ((const if_event_msg_t *)msg->payload)->event;
            }
            if (if_event == IF_EVENT_SMOOTHEND)
            {
                ldp_handle_if_smoothend();
            }
            if (ldp_worker_post_if_event(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;
        }

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
     *   2. worker 启动
     *   3. 订阅所有依赖（含 CLI；TUNNEL 因 LDP 必需用 auto_start=1）
     *   4. wait_all_subscribed_connected：等所有 peer IPC 都 CONNECTED
     *   5. db_init + db_restore
     *   6. notify_ready：业务真正可用 */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: timed out waiting for DEV connection; module may be unusable");
    }

    if (ldp_worker_prepare() != ERRCODE_SUCCESS || ldp_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: worker start failed");
        ldp_worker_shutdown();
        return -1;
    }

    /* 显式依赖：使能 LDP 即拉起 TUNNEL（按需模块，否则保持 idle）。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_TUNNEL, 1, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: subscribe(TUNNEL) failed; MPLS forwarding may not work");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0, ldp_on_route_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: subscribe(ROUTE) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0, ldp_on_if_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: subscribe(IF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, ldp_on_db_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: subscribe(DB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: subscribe(CLI) failed");
    }

    /* DEPS_READY：阻塞直到所有订阅 peer 的 IPC 都 CONNECTED 才继续 db_init/restore + notify_ready。
     * 不能用超时后继续 —— DEV 视角 READY 而 CFG 还连不上 LDP 会让命令派发踩到 race。 */
    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0);

    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        ldp_handle_db_ready();
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

    /* 先 dev_ipc_destroy 停掉 IPC 派发线程，再 worker_shutdown 释放 worker 本地状态。
     * 否则 IPC 派发到 ldp_worker_post_* 会访问已置 NULL 的全局指针 → SEGV。 */
    dev_ipc_context_t *ctx = g_ldp_local->dev_ipc_ctx;
    g_ldp_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    ldp_worker_shutdown();

    g_free(g_ldp_local);
    g_ldp_local = NULL;
}
