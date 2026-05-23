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

void bgp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case BGP_MSG_TYPE_INTERNAL_IF_READY:
            bgp_handle_if_ready();
            break;
        case BGP_MSG_TYPE_INTERNAL_VRF_READY:
            bgp_handle_vrf_ready();
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
                /* DB 不可用时直接拒绝配置下发，避免内存/OS 与 DB 静默偏移 */
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

    /* DB：只 init 建表，restore 推迟到 subscribes 之后（依赖 cache） */
    if (dev_ipc_wait_module_ready(ctx, DEV_MODULE_ID_DB, 5000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: DB not ready, skip db init");
    }
    else if (bgp_db_init() != 0)
    {
        LOG_ERROR("BGP: DB init failed");
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

    /* 现在 worker 已 ready，可以安全订阅（订阅 → 事件 → bgp_worker_post_*） */

    /* TUNNEL 用 auto_start=0：BGP 不硬依赖 TUNNEL（纯 IPv4-unicast 用不到 MPLS）。
     * 用户配 labeled / VPN 地址族时由 CLI handler (handle_bgp_addr_family) 显式触发 TUNNEL 启动。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_TUNNEL, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(TUNNEL) failed");
    }

    /* VRF：auto_start=1 触发 + cb 在每次 VRF READY 时重新订阅事件 */
    /* VRF 用 auto_start=0：BGP 默认 VRF 不需要 VRF 模块；用户配 VRF AF 时再由 VRF 命令触发拉起 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_VRF, 0, bgp_on_vrf_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(VRF) failed");
    }

    /* ROUTE：基础模块，每次 READY 后由 worker 重订阅/重注册/重下刷。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0, bgp_on_route_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(ROUTE) failed");
    }

    /* IF：用回调模式，IF 每次 READY 触发 worker 重新订阅事件 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0, bgp_on_if_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(IF) failed");
    }

    /* db_restore 放最后（subscribes 已触发缓存回放） */
    if (bgp_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: DB restore failed");
    }

    /* subscribe(CLI) 最后：CFG poll is_connected(BGP)=true 时 BGP 已 fully ready */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("BGP: subscribe(CLI) failed");
    }

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

    bgp_bmp_thread_shutdown();
    bgp_worker_shutdown();

    dev_ipc_context_t *ctx = g_bgp_local->dev_ipc_ctx;
    g_bgp_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    if (!g_bgp_local)
    {
        return;
    }

    g_free(g_bgp_local);
    g_bgp_local = NULL;
}
