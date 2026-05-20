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

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_BGP,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
    (void)result;
}

static void bgp_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");
    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    dev_ipc_connect(ctx, DEV_MODULE_ID_ROUTE, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_ROUTE);
    dev_ipc_connect(ctx, DEV_MODULE_ID_TUNNEL, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_TUNNEL);
    dev_ipc_connect(ctx, DEV_MODULE_ID_IF, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_IF);
    dev_ipc_connect(ctx, DEV_MODULE_ID_VRF, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_VRF);
    LOG_INFO("Connected to CFG, DB, ROUTE, TUNNEL, IF and VRF");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

static void bgp_on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

static void bgp_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Initializing database tables and restoring BGP state");

    if (bgp_db_init() != 0)
    {
        LOG_ERROR("BGP: Database table initialization failed");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    if (bgp_worker_prepare() != ERRCODE_SUCCESS)
    {
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    /* bgp_worker 线程必须先启动，restore 通过 bgp_worker_dispatch_apply() 向其派发命令 */
    if (bgp_worker_launch() != ERRCODE_SUCCESS)
    {
        bgp_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    /* BMP 线程初始化并启动 */
    if (bgp_bmp_thread_prepare() != ERRCODE_SUCCESS)
    {
        bgp_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    if (bgp_bmp_thread_launch() != ERRCODE_SUCCESS)
    {
        bgp_bmp_thread_shutdown();
        bgp_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    /* 通过 if_api 订阅 IF 全量事件，用于维护统一接口缓存 */
    if (if_api_subscribe_all(ctx) == ERRCODE_SUCCESS)
    {
        LOG_INFO("BGP: Subscribed to IF events via if_api (ALL types, ALL events)");
    }
    else
    {
        LOG_WARN("BGP: Failed to subscribe to IF events via if_api");
    }

    /* 通过 vrf_api 订阅 VRF 事件（VRF 删除 + RD/RT 增删），并请求初始全量回放 */
    vrf_api_cache_init();
    uint32_t vrf_event_mask = VRF_EVENT_VRF_ADD | VRF_EVENT_VRF_DEL | VRF_EVENT_AF_ENABLE | VRF_EVENT_AF_DISABLE |
                              VRF_EVENT_AF_RD_ADD | VRF_EVENT_AF_RD_DEL | VRF_EVENT_AF_IMPORT_RT_ADD |
                              VRF_EVENT_AF_IMPORT_RT_DEL | VRF_EVENT_AF_EXPORT_RT_ADD | VRF_EVENT_AF_EXPORT_RT_DEL;
    if (vrf_api_subscribe(ctx, VRF_AF_MASK_ALL, vrf_event_mask, VRF_SUBSCRIBE_FLAG_REPLAY) == ERRCODE_SUCCESS)
    {
        LOG_INFO("BGP: Subscribed to VRF events via vrf_api (REPLAY)");
    }
    else
    {
        LOG_WARN("BGP: Failed to subscribe to VRF events via vrf_api");
    }

    /* 仅恢复：表不存在（BGP 未曾配置）时静默返回 NULL，不建表也不写默认值 */
    uint32_t ret = bgp_db_restore();
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: Failed to restore state from database");
        bgp_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    LOG_INFO("BGP worker thread started");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

void bgp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            bgp_on_start(msg);
            return;

        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            bgp_on_connect(msg);
            return;

        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            bgp_on_ready(msg);
            return;
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
