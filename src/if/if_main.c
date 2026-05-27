/**
 * @file   if_main.c
 * @brief  接口模块主入口：三阶段初始化与 IPC 消息分发
 * @author jhb
 * @date   2026/01/22
 */
#include "if_main.h"

#include <glib.h>
#include <stddef.h>
#include <stdlib.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "if.h"
#include "if_bdr.h"
#include "if_cli.h"
#include "if_db.h"
#include "if_link_monitor.h"
#include "log.h"
#include "vrf.h"
#include "work/if_worker.h"

if_local_t *g_if_local = NULL;

// ============================================================================
// 依赖就绪回调（含初次 + 重启）
// ============================================================================

static void if_on_vrf_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                               void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (!g_if_local || !g_if_local->dev_ipc_ctx)
    {
        return;
    }
    uint32_t msg_type;
    if (event == DEV_MODULE_EVENT_READY)
    {
        msg_type = IF_MSG_TYPE_INTERNAL_VRF_READY;
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        msg_type = IF_MSG_TYPE_INTERNAL_VRF_DOWN;
    }
    else
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(msg_type, DEV_MODULE_ID_IF, DEV_MODULE_ID_IF, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_if_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void if_on_route_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
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
    if (!g_if_local || !g_if_local->dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m =
        dev_ipc_message_create(IF_MSG_TYPE_INTERNAL_ROUTE_READY, DEV_MODULE_ID_IF, DEV_MODULE_ID_IF, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_if_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void if_on_ipc_disconnect(dev_ipc_context_t *ctx, uint32_t remote_module_id, void *user)
{
    (void)ctx;
    (void)user;

    if (if_worker_post_module_down(remote_module_id) != ERRCODE_SUCCESS)
    {
        LOG_DEBUG("IF: skip module-down cleanup for 0x%08X", remote_module_id);
    }
}

static void if_handle_vrf_ready(void)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_VRF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: VRF not connected in time; vrf_api_subscribe deferred");
        return;
    }
    if (vrf_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: vrf_api_subscribe_all failed");
    }
    else
    {
        LOG_INFO("IF: subscribed to VRF events");
    }
}

static void if_handle_route_ready(void)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_ROUTE, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: ROUTE not connected in time; connected route replay deferred");
        return;
    }
    if (if_worker_post_route_ready() != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: failed to post ROUTE-ready replay");
    }
}

static gboolean g_if_db_restored = FALSE;
static gboolean g_if_link_monitor_started = FALSE;
static gboolean g_if_vrf_smoothend = FALSE; /* VRF REPLAY 已完成 */

static void if_try_db_restore(void)
{
    if (g_if_db_restored)
    {
        return;
    }
    if (!g_if_vrf_smoothend)
    {
        return;
    }

    if (if_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: db restore failed");
        return;
    }
    g_if_db_restored = TRUE;
    /* 通知 worker：restore 已结束。worker 收到后会把 pending_replay 的订阅者
     * 统一补发 SMOOTHSTART/REPLAY/SMOOTHEND，避免推空数据。 */
    (void)if_worker_post_restore_done();
    LOG_INFO("IF: DB restore completed");
}

static void if_handle_db_ready(void)
{
    /* DB MODULE_EVENT READY 触发：等握手完成（subscribe / event 只是触发 connect，IO 线程异步建联）。
     * db_init 幂等；link_monitor 只起一次。 */
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: DB not connected in time; db restore deferred");
        return;
    }

    if (if_db_init() != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: db init failed");
        return;
    }
    if (!g_if_link_monitor_started)
    {
        if (if_link_monitor_start() != 0)
        {
            LOG_WARN("IF: link monitor start failed, link recovery disabled");
        }
        g_if_link_monitor_started = TRUE;
    }

    if_try_db_restore();
}

static void if_handle_vrf_smoothend(void)
{
    gboolean first = !g_if_vrf_smoothend;
    g_if_vrf_smoothend = TRUE;

    if (first)
    {
        LOG_INFO("IF: VRF smoothend received (initial sync)");
        if_try_db_restore();
        return;
    }

    /* VRF 进程重启后的再同步：worker 已在 SMOOTHSTART 时清掉非 public VRF 的接口内存绑定，
     * 这里只从 DB 重恢复 vrf_name 非空的接口行。 */
    LOG_INFO("IF: VRF smoothend received (resync)");
    (void)if_db_restore_vrf_bound();
}

static void if_on_db_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                              void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event != DEV_MODULE_EVENT_READY || !g_if_local || !g_if_local->dev_ipc_ctx)
    {
        return;
    }

    dev_ipc_message_t *m =
        dev_ipc_message_create(IF_MSG_TYPE_INTERNAL_DB_READY, DEV_MODULE_ID_IF, DEV_MODULE_ID_IF, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_if_local->dev_ipc_ctx->msg_queue, m);
    }
}

// ============================================================================
// IPC 消息分发
// ============================================================================

void if_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        /* ---- show current-configuration：IPC 线程直接读 DB 生成 ---- */
        case CLI_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("Received show current-configuration request");
            if_bdr_show_config(msg);
            dev_ipc_message_free(msg);
            return;

        /* ---- IF 订阅应答：静默丢弃 ---- */
        case IF_MSG_TYPE_ACK:
            dev_ipc_message_free(msg);
            return;

        /* ---- 内部：VRF 模块就绪 → worker 线程做 vrf_api_subscribe_all ---- */
        case IF_MSG_TYPE_INTERNAL_VRF_READY:
            if_handle_vrf_ready();
            dev_ipc_message_free(msg);
            return;

        /* ---- 内部：VRF 模块 DOWN → worker 清接口绑定 + 清 vrf_api cache ---- */
        case IF_MSG_TYPE_INTERNAL_VRF_DOWN:
            (void)if_worker_post_vrf_down();
            dev_ipc_message_free(msg);
            return;

        case IF_MSG_TYPE_INTERNAL_DB_READY:
            if_handle_db_ready();
            dev_ipc_message_free(msg);
            return;

        case IF_MSG_TYPE_INTERNAL_ROUTE_READY:
            if_handle_route_ready();
            dev_ipc_message_free(msg);
            return;

        case VRF_MSG_TYPE_EVENT:
        {
            /* VRF 事件到达后，唤醒等待该 VRF 出现的挂起项。
             * 用 offsetof(rts) 而非 sizeof(vrf_event_msg_t)——后者多算了末尾变长数组的一个元素，
             * 对 rt_count=0 的 ADD/STATE/AF_ENABLE 事件会误判为载荷不足。 */
            char vrf_name[VRF_NAME_MAX_LEN] = "";
            uint32_t vrf_event = 0;
            if (msg->payload && msg->payload_len >= offsetof(vrf_event_msg_t, rts))
            {
                const vrf_event_msg_t *evt = (const vrf_event_msg_t *)msg->payload;
                vrf_event = evt->event;
                g_strlcpy(vrf_name, evt->name, sizeof(vrf_name));
            }
            if (vrf_event == VRF_EVENT_SMOOTHEND)
            {
                if_handle_vrf_smoothend();
            }
            if (if_worker_dispatch_vrf_event(msg) != ERRCODE_SUCCESS)
            {
                LOG_WARN("IF: failed to dispatch VRF event to worker");
                dev_ipc_message_free(msg);
                return;
            }
            (void)vrf_name;
            return;
        }

        case VRF_MSG_TYPE_ACK:
            dev_ipc_message_free(msg);
            return;

        /* ---- CLI 命令：show 转 worker，配置在 IPC 线程解析后 dispatch apply ---- */
        case CLI_MSG_TYPE:
        {
            uint8_t flags = 0;
            if (msg->payload && msg->payload_len >= 1)
            {
                flags = ((const uint8_t *)msg->payload)[0];
            }
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (if_worker_post_ipc_message(msg) != ERRCODE_SUCCESS)
                {
                    LOG_WARN("IF: failed to post show cmd to worker");
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                /* DB 不在线时拒绝配置：避免内存改了 / DB 写不到的静默偏移 */
                if (db_rpc_guard_reject(ctx, msg, "IF"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
                if_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }

        /* ---- show 分片续传 / 候选查询 / 订阅：交给 worker（共享 show_stream 与业务数据） ---- */
        case CLI_MSG_TYPE_CONTINUE:
        case CLI_MSG_TYPE_QUERY_CANDIDATES:
        case IF_MSG_TYPE_SUBSCRIBE:
        case IF_MSG_TYPE_UNSUBSCRIBE:
            if (if_worker_post_ipc_message(msg) != ERRCODE_SUCCESS)
            {
                LOG_WARN("IF: failed to post msg 0x%08X to worker", msg->msg_type);
                dev_ipc_message_free(msg);
            }
            return;

        default:
            LOG_WARN("IF: received unknown message type: 0x%08X", msg->msg_type);
            dev_ipc_message_free(msg);
            return;
    }
}

// ============================================================================
// Module initialization
// ============================================================================

int if_module_init(void)
{
    log_set_tag("if");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_IF, "if", DEV_MODULE_PORT_IF, if_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_if_local = g_malloc0(sizeof(if_local_t));
    if (!g_if_local)
    {
        LOG_ERROR("Failed to allocate IF local context");
        dev_ipc_destroy(ctx);
        return -1;
    }
    g_if_local->dev_ipc_ctx = ctx;
    dev_ipc_set_disconnect_handler(ctx, if_on_ipc_disconnect, NULL);

    /* 弱依赖模型 init：
     *   1. 等 DEV 控制连接
     *   2. worker 启动（先于触发回调的 subscribes，避免 race）
     *   3. subscribe(VRF, auto_start=1, cb) 触发 VRF + 注册重启感知
     *   4. db_init + restore + link_monitor 启动
     *   5. subscribe(CLI) 末尾
     *   6. notify_ready */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: timed out waiting for DEV connection");
    }

    if (if_worker_prepare() != ERRCODE_SUCCESS || if_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: worker start failed");
        if_worker_shutdown();
        return -1;
    }

    /* 一次性订阅所有依赖（CLI 也含在内；CFG 卡 PHASE=READY，与 CLI 订阅顺序无关）*/
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_VRF, 0, if_on_vrf_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: subscribe(VRF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0, if_on_route_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: subscribe(ROUTE) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, if_on_db_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: subscribe(DB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: subscribe(CLI) failed");
    }

    /* DEPS_READY：阻塞直到所有订阅 peer 都 CONNECTED 再继续 DB 恢复 + notify_ready。
     * 不能用超时后继续 —— DEV 视角 READY 而 CFG 还连不上 IF 会让命令派发踩到 race。 */
    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0);

    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        if_handle_db_ready();
    }

    /* 业务已就绪，进入 READY 阶段；CFG 此后才会派 config */
    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: notify_ready to DEV failed");
    }

    LOG_INFO("IF: module ready");

    return 0;
}

void if_module_cleanup(void)
{
    /* 优雅退出顺序：
     *   1. if_worker_pre_shutdown_cleanup：清 OS netlink IP + 通知 ROUTE。此时 worker
     *      仍要处理 dispatch + 通过 IPC 发 RPC，必须放最前。
     *   2. dev_ipc_destroy：停掉 IPC 派发线程，避免后续 worker_shutdown 释放 g_if_work_local
     *      之后 IPC 派发到 if_worker_post_* 触发 NULL 解引用 SEGV。
     *   3. if_link_monitor_stop / if_worker_shutdown：拆掉 worker 自身资源。 */
    (void)if_worker_pre_shutdown_cleanup();

    if (g_if_local)
    {
        dev_ipc_context_t *ctx = g_if_local->dev_ipc_ctx;
        g_if_local->dev_ipc_ctx = NULL;
        if (ctx)
        {
            dev_ipc_destroy(ctx);
        }
    }

    if_link_monitor_stop();
    if_worker_shutdown();

    if (!g_if_local)
    {
        return;
    }
    g_free(g_if_local);
    g_if_local = NULL;
}
