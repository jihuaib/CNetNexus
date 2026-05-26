/**
 * @file   bgp_main.c
 * @brief  BGP 模块主入口：生命周期处理与消息分发
 * @author jhb
 * @date   2026/01/22
 */
#include "bgp_main.h"

#include <arpa/inet.h>
#include <string.h>

#include "bgp.h"
#include "bgp_attr_intern.h"
#include "bgp_bdr.h"
#include "bgp_bmp_cli.h"
#include "bgp_bmp_thread.h"
#include "bgp_cli.h"
#include "bgp_db.h"
#include "bgp_pkt.h"
#include "bgp_worker.h"
#include "db.h"
#include "errcode.h"
#include "if.h"
#include "log.h"
#include "route.h"
#include "tunnel.h"
#include "vrf.h"

bgp_local_t *g_bgp_local;

static bgp_afi_t bgp_vrf_event_map_afi(uint16_t afi)
{
    if (afi == VRF_AFI_IPV6)
    {
        return BGP_AFI_IPV6;
    }
    return BGP_AFI_IPV4;
}

static bgp_safi_t bgp_vrf_event_map_safi(uint8_t safi)
{
    return (safi == VRF_SAFI_UNICAST) ? BGP_SAFI_UNICAST : (bgp_safi_t)safi;
}

static void bgp_handle_vrf_event_db_side_effect(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < offsetof(vrf_event_msg_t, rts))
    {
        return;
    }

    const vrf_event_msg_t *evt = (const vrf_event_msg_t *)msg->payload;
    if (evt->name[0] == '\0' || strcmp(evt->name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        return;
    }

    switch (evt->event)
    {
        case VRF_EVENT_VRF_DEL:
            (void)bgp_db_del_vrf(evt->name);
            break;

        case VRF_EVENT_AF_DISABLE:
            (void)bgp_db_del_instance(evt->name, bgp_vrf_event_map_afi(evt->afi), bgp_vrf_event_map_safi(evt->safi));
            break;

        case VRF_EVENT_AF_RD_DEL:
            (void)bgp_db_del_instance(evt->name, bgp_vrf_event_map_afi(evt->afi), bgp_vrf_event_map_safi(evt->safi));
            break;

        default:
            break;
    }
}

static uint8_t bgp_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

/**
 * @brief 从 CLI 载荷中提取 group_id（偏移 1 处的 uint32 网络序）
 */
static uint32_t bgp_cli_payload_group_id(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 5)
    {
        return 0;
    }
    uint32_t gid;
    memcpy(&gid, (const uint8_t *)msg->payload + 1, 4);
    return ntohl(gid);
}

/**
 * @brief 判断 group_id 是否属于 BMP 配置命令范围
 */
static gboolean bgp_is_bmp_group(uint32_t group_id)
{
    return group_id >= BGP_CLI_GROUP_ID_BMP_INSTANCE && group_id <= BGP_CLI_GROUP_ID_BMP_MONITOR;
}

/* ============================================================================
 * 依赖就绪回调（IF / VRF 重启后自动重新订阅事件）
 * ============================================================================ */

static void bgp_post_internal(uint32_t msg_type)
{
    if (!g_bgp_local || !g_bgp_local->dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(msg_type, DEV_MODULE_ID_BGP, DEV_MODULE_ID_BGP, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_bgp_local->dev_ipc_ctx->msg_queue, m);
    }
}

/* IF dep 事件回调：
 *   READY → 投递 IF_READY，worker 做 if_api_subscribe_all。
 *   DOWN  → 投递 IF_DOWN，worker 清 IF 缓存、拆 source-if 绑定的会话。
 *           路由 nexthop 可达性由 ROUTE 侧重算后通过 NH_NOTIFY 主动通知 BGP，
 *           无需在此重注册（重注册仅用于 ROUTE 进程重启后 watch 丢失的场景）。 */
static void bgp_on_if_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                               void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        bgp_post_internal(BGP_MSG_TYPE_INTERNAL_IF_READY);
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        bgp_post_internal(BGP_MSG_TYPE_INTERNAL_IF_DOWN);
    }
}

static void bgp_on_vrf_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        bgp_post_internal(BGP_MSG_TYPE_INTERNAL_VRF_READY);
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        bgp_post_internal(BGP_MSG_TYPE_INTERNAL_VRF_DOWN);
    }
}

static void bgp_on_route_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                  void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        bgp_post_internal(BGP_MSG_TYPE_INTERNAL_ROUTE_READY);
    }
}

static void bgp_handle_if_ready(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, 3000) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: IF not connected within 3s; if_api_subscribe_all deferred");
        return;
    }
    if (if_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: if_api_subscribe_all failed");
    }
    else
    {
        LOG_INFO("BGP: subscribed to IF events");
    }
}

static void bgp_handle_vrf_ready(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_VRF, 3000) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: VRF not connected within 3s; vrf_api_subscribe deferred");
        return;
    }
    uint32_t vrf_event_mask = VRF_EVENT_VRF_ADD | VRF_EVENT_VRF_DEL | VRF_EVENT_AF_ENABLE | VRF_EVENT_AF_DISABLE |
                              VRF_EVENT_AF_RD_ADD | VRF_EVENT_AF_RD_DEL | VRF_EVENT_AF_IMPORT_RT_ADD |
                              VRF_EVENT_AF_IMPORT_RT_DEL | VRF_EVENT_AF_EXPORT_RT_ADD | VRF_EVENT_AF_EXPORT_RT_DEL;
    if (vrf_api_subscribe(ctx, VRF_AF_MASK_ALL, vrf_event_mask, VRF_SUBSCRIBE_FLAG_REPLAY) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: vrf_api_subscribe failed");
    }
    else
    {
        LOG_INFO("BGP: subscribed to VRF events (with REPLAY)");
    }
}

static gboolean g_bgp_db_restored = FALSE;
static gboolean g_bgp_vrf_smoothend = FALSE; /* VRF REPLAY 已完成（缓存就绪） */
static gboolean g_bgp_if_smoothend = FALSE;  /* IF REPLAY 已完成（接口缓存就绪） */

static void bgp_try_db_restore(void)
{
    if (g_bgp_db_restored)
    {
        return;
    }
    if (!g_bgp_vrf_smoothend || !g_bgp_if_smoothend)
    {
        return;
    }

    if (bgp_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: DB restore failed");
        return;
    }
    g_bgp_db_restored = TRUE;
    LOG_INFO("BGP: DB restore completed");
}

static void bgp_handle_db_ready(void)
{
    /* DB MODULE_EVENT READY 触发：等握手完成（subscribe / event 只是触发 connect，IO 线程异步建联），
     * 然后无条件 db_init（CREATE TABLE IF NOT EXISTS 幂等；DB 重启后若丢了状态可重建表）。 */
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, 3000) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: DB not connected within 3s; db restore deferred");
        return;
    }

    if (bgp_db_init() != 0)
    {
        LOG_ERROR("BGP: DB init failed");
        return;
    }
    bgp_try_db_restore();
}

static void bgp_handle_vrf_smoothend(void)
{
    gboolean first = !g_bgp_vrf_smoothend;
    g_bgp_vrf_smoothend = TRUE;

    if (first)
    {
        LOG_INFO("BGP: VRF smoothend received (initial sync)");
        bgp_try_db_restore();
        return;
    }

    /* VRF 进程重启后的再同步：worker 已在 SMOOTHSTART 时拆除依赖 VRF 的 bgp_vrf_t，
     * 这里只从 DB 重恢复 vrf_name 非 public 的行（vrf/session/instance/neighbor/qp_route）。 */
    LOG_INFO("BGP: VRF smoothend received (resync)");
    (void)bgp_db_restore_vrf_bound();
}

static void bgp_handle_if_smoothend(void)
{
    gboolean first = !g_bgp_if_smoothend;
    g_bgp_if_smoothend = TRUE;

    if (first)
    {
        LOG_INFO("BGP: IF smoothend received (initial sync)");
        bgp_try_db_restore();
    }
    else
    {
        LOG_INFO("BGP: IF smoothend received (resync)");
    }
}

static void bgp_on_db_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                               void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event != DEV_MODULE_EVENT_READY || !g_bgp_local)
    {
        return;
    }

    bgp_post_internal(BGP_MSG_TYPE_INTERNAL_DB_READY);
}

void bgp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    /* cleanup 阶段:worker 已经/正在销毁,直接丢弃避免 worker_post_* 撞 NULL g_bgp_work_local */
    if (g_bgp_local && g_bgp_local->shutting_down)
    {
        dev_ipc_message_free(msg);
        return;
    }

    switch (msg->msg_type)
    {
        case BGP_MSG_TYPE_INTERNAL_DB_READY:
            bgp_handle_db_ready();
            break;
        case BGP_MSG_TYPE_INTERNAL_IF_READY:
            bgp_handle_if_ready();
            break;
        case BGP_MSG_TYPE_INTERNAL_VRF_READY:
            bgp_handle_vrf_ready();
            break;
        case BGP_MSG_TYPE_INTERNAL_VRF_DOWN:
            if (bgp_worker_post_vrf_down() != 0)
            {
                LOG_WARN("BGP: failed to post VRF-down purge to worker");
            }
            break;
        case BGP_MSG_TYPE_INTERNAL_ROUTE_READY:
            if (bgp_worker_post_route_ready() != 0)
            {
                LOG_WARN("BGP: failed to post ROUTE-ready replay");
            }
            break;
        case BGP_MSG_TYPE_INTERNAL_IF_DOWN:
            if (bgp_worker_post_if_down() != 0)
            {
                LOG_WARN("BGP: failed to post IF-down teardown");
            }
            break;

        case CLI_MSG_TYPE:
        {
            uint8_t flags = bgp_cli_payload_flags(msg);
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                uint32_t gid = bgp_cli_payload_group_id(msg);
                if (gid == BGP_CLI_GROUP_ID_BMP_SHOW)
                {
                    if (bgp_bmp_thread_post_show(msg) != 0)
                    {
                        LOG_WARN("BGP: Failed to forward BMP show command to BMP thread");
                        dev_ipc_message_free(msg);
                    }
                }
                else if (bgp_worker_post_show_cli(msg) != 0)
                {
                    LOG_WARN("BGP: Failed to forward CLI show command to worker thread");
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                /* DB 不在线时拒绝配置：避免内存改了 / DB 写不到的静默偏移 */
                if (db_rpc_guard_reject(ctx, msg, "BGP"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
                uint32_t gid = bgp_cli_payload_group_id(msg);
                if (bgp_is_bmp_group(gid))
                {
                    bgp_bmp_cli_handle_config_msg(msg);
                }
                else
                {
                    bgp_cli_handle_config_msg(msg);
                }
                dev_ipc_message_free(msg);
            }
            return;
        }
        case CLI_MSG_TYPE_SHOW_CONFIG:
        {
            bgp_bdr_show_config(msg);
            break;
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

        case TUNNEL_MSG_TYPE_RESOLVE_NOTIFY:
        {
            if (bgp_worker_post_tunnel_message(msg) != 0)
            {
                LOG_WARN("BGP: Failed to forward tunnel message to worker thread (type=0x%08X)", msg->msg_type);
                dev_ipc_message_free(msg);
            }
            return;
        }

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
                bgp_handle_if_smoothend();
            }
            if (bgp_worker_post_if_event(msg) != 0)
            {
                LOG_WARN("BGP: Failed to forward IF event to worker thread");
                dev_ipc_message_free(msg);
            }
            return;
        }
        case IF_MSG_TYPE_ACK:
            /* IF 订阅应答，静默丢弃 */
            break;

        /* ---- VRF 事件通知 ---- */
        case VRF_MSG_TYPE_EVENT:
        {
            /* 平滑同步结束：触发 DB restore（若 DB 也已就绪）。
             * 仍然转发给 worker，让 cache_on_event 走过；hit default 分支即丢弃。 */
            uint32_t vrf_event = 0;
            if (msg->payload && msg->payload_len >= offsetof(vrf_event_msg_t, rts))
            {
                vrf_event = ((const vrf_event_msg_t *)msg->payload)->event;
            }
            if (vrf_event == VRF_EVENT_SMOOTHEND)
            {
                bgp_handle_vrf_smoothend();
            }
            bgp_handle_vrf_event_db_side_effect(msg);
            if (bgp_worker_post_vrf_event(msg) != 0)
            {
                LOG_WARN("BGP: Failed to forward VRF event to worker thread");
                dev_ipc_message_free(msg);
            }
            return;
        }
        case VRF_MSG_TYPE_ACK:
            /* VRF 订阅应答，静默丢弃 */
            break;

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

int bgp_module_init(void)
{
    log_set_tag("bgp");
    LOG_INFO("Module initialization");

    bgp_parse_init();
    bgp_pkt_build_init();
    bgp_attr_intern_init();

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

    /* 弱依赖模型启动（顺序至关重要）：
     *   1. 等 DEV 控制连接
     *   2. wait_module_ready(DB) → db_init（建表）
     *   3. worker + BMP 线程启动（之后才能安全处理 IF/VRF/ROUTE 事件）
     *   4. vrf_api cache 初始化（msg_handler 收到 VRF_EVENT 时会用）
     *   5. 各种 subscribe：触发依赖拉起 + 注册重启回调
     *      （此时 worker 已 ready，事件回调到达不会 crash）
     *   6. db_restore（依赖 vrf cache 等已就绪）
     *   7. subscribe(CLI) 放最后：CFG 看到本模块在跑即可立即 dispatch
     *   8. notify_ready */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, 10000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: timed out waiting for DEV connection; module may be unusable");
    }

    if (bgp_worker_prepare() != ERRCODE_SUCCESS || bgp_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: worker start failed");
        bgp_worker_shutdown();
        return -1;
    }
    if (bgp_bmp_thread_prepare() != ERRCODE_SUCCESS || bgp_bmp_thread_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: BMP thread start failed");
        bgp_bmp_thread_shutdown();
        bgp_worker_shutdown();
        return -1;
    }

    vrf_api_cache_init();

    /* 一次性订阅所有依赖（含 CLI）。订阅顺序不影响 CFG 的派发时机——
     * CFG 卡在 DEV 的 PHASE=READY，subscribe(CLI) 早晚都不会让 CFG 提前下发 config。
     * 注意：TUNNEL 不在这里订阅；它由 labeled / VPN AF 的配置路径在用到时按需
     * wait_module_ready 拉起（见 bgp_cli.c）。常驻订阅一个未运行的按需模块会让
     * wait_all_subscribed_connected 一直等到 10s 超时。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_VRF, 0, bgp_on_vrf_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(VRF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0, bgp_on_route_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(ROUTE) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0, bgp_on_if_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(IF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, bgp_on_db_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(DB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(CLI) failed");
    }

    /* DEPS_READY：等所有订阅 peer 的 IPC 都 CONNECTED 才继续 db_init/restore */
    if (dev_ipc_wait_all_subscribed_connected(ctx, 10000) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: deps not fully connected within 10s; proceeding anyway");
    }

    if (bgp_db_init() != 0)
    {
        LOG_ERROR("BGP: DB init failed");
    }
    else if (bgp_db_restore() == ERRCODE_SUCCESS)
    {
        /* 首次 restore：VRF-bound 行此时 vrf_api cache 可能为空被跳过，
         * 后续 VRF smoothend 会触发 bgp_db_restore_vrf_bound 补齐 */
        g_bgp_db_restored = TRUE;
        LOG_INFO("BGP: initial DB restore done");
    }

    /* 业务状态已恢复，进入 READY 阶段；CFG 此后才会派 config */
    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: notify_ready to DEV failed");
    }

    LOG_INFO("BGP: module ready");

    return 0;
}

void bgp_module_cleanup(void)
{
    if (!g_bgp_local)
    {
        return;
    }

    /* 1) 置 shutting_down,msg_handler 后续丢弃所有消息,避免 worker 销毁过程中
     *    被 bgp_worker_post_if_event 等访问 NULL g_bgp_work_local。 */
    g_bgp_local->shutting_down = 1;

    /* 2) worker 仍可用 + IPC 仍可用 → 给业务一次机会做 graceful withdraw
     *    (撤销 BGP 注入到 ROUTE 的路由)。原顺序在 worker_shutdown 前就 dev_ipc_destroy,
     *    导致 worker 里通过 IPC 发的撤销 RPC 全部失败。 */
    bgp_bmp_thread_shutdown();
    bgp_worker_shutdown();

    /* 3) 关 IPC,join IO/worker 线程,断所有连接。 */
    dev_ipc_context_t *ctx = g_bgp_local->dev_ipc_ctx;
    g_bgp_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_bgp_local);
    g_bgp_local = NULL;
}
