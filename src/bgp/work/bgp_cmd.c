/**
 * @file   bgp_cmd.c
 * @brief  BGP worker 命令队列实现（外部线程 -> worker 线程的 RPC 通道）
 * @author jhb
 * @date   2026/04/17
 *
 * 支持 5 种命令：
 *   SHOW_CLI     — CLI 线程投递 show 命令
 *   ROUTE_MSG    — IPC 线程投递 ROUTE 模块消息（update/report/nh-notify）
 *   TUNNEL_MSG   — IPC 线程投递 TUNNEL 模块解析通知
 *   IF_EVENT     — IPC 线程投递 IF 接口事件
 *   APPLY        — CLI/DB 线程投递同步配置应用命令（waitable）
 *   SHUTDOWN     — bgp_worker_shutdown 投递的退出信号（waitable）
 */
#include "bgp_cmd.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bgp_apply_vrf.h"
#include "bgp_cfg_apply.h"
#include "bgp_cli.h"
#include "bgp_import_rib.h"
#include "bgp_import_route.h"
#include "bgp_main.h"
#include "bgp_nexthop.h"
#include "bgp_protocol.h"
#include "bgp_relay.h"
#include "bgp_route_flush.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_vrf_import.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "if.h"
#include "log.h"
#include "route.h"
#include "tunnel.h"
#include "vrf.h"

/** epoll data.ptr sentinel */
char bgp_cmd_tag;

// ============================================================================
// 内部类型
// ============================================================================

typedef enum bgp_cmd_type
{
    BGP_CMD_TYPE_SHOW_CLI = 1,    /**< show CLI 命令派发（CLI_MSG_TYPE/CLI_MSG_TYPE_CONTINUE） */
    BGP_CMD_TYPE_SHUTDOWN = 2,    /**< worker 退出信号 */
    BGP_CMD_TYPE_APPLY = 3,       /**< 跨线程配置应用命令 */
    BGP_CMD_TYPE_ROUTE_MSG = 4,   /**< ROUTE_MSG_TYPE_UPDATE/REPORT/NH_NOTIFY */
    BGP_CMD_TYPE_IF_EVENT = 5,    /**< IF 接口事件（IF_MSG_TYPE_EVENT） */
    BGP_CMD_TYPE_TUNNEL_MSG = 6,  /**< TUNNEL_MSG_TYPE_RESOLVE_NOTIFY */
    BGP_CMD_TYPE_VRF_EVENT = 7,   /**< VRF_MSG_TYPE_EVENT */
    BGP_CMD_TYPE_ROUTE_READY = 8, /**< ROUTE READY/restart 后重订阅/重注册/重下刷 */
    BGP_CMD_TYPE_IF_DOWN = 9,     /**< IF 模块下线，清缓存 + 拆 source-if 会话 + 重注册 nexthop */
    BGP_CMD_TYPE_VRF_DOWN = 10,   /**< VRF 模块下线，拆非 public bgp_vrf_t + 清 vrf_api cache */
} bgp_cmd_type_t;

typedef struct bgp_cmd
{
    bgp_cmd_type_t type;
    dev_ipc_message_t *msg; /**< SHOW_CLI / ROUTE_MSG / IF_EVENT */
    bgp_apply_cmd_t *apply; /**< APPLY（借用引用，不持有所有权） */
    gboolean waitable;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    gboolean done;
    int rc;
} bgp_cmd_t;

// ============================================================================
// 生命周期 + 队列基础操作
// ============================================================================

static bgp_cmd_t *bgp_cmd_create(bgp_cmd_type_t type, dev_ipc_message_t *msg, gboolean waitable)
{
    bgp_cmd_t *cmd = g_malloc0(sizeof(*cmd));
    if (!cmd)
    {
        return NULL;
    }

    cmd->type = type;
    cmd->msg = msg;
    cmd->waitable = waitable;
    if (waitable)
    {
        pthread_mutex_init(&cmd->mutex, NULL);
        pthread_cond_init(&cmd->cond, NULL);
    }
    return cmd;
}

static void bgp_cmd_destroy(bgp_cmd_t *cmd)
{
    if (!cmd)
    {
        return;
    }
    if (cmd->waitable)
    {
        pthread_mutex_destroy(&cmd->mutex);
        pthread_cond_destroy(&cmd->cond);
    }
    g_free(cmd);
}

static void bgp_cmd_complete(bgp_cmd_t *cmd, int rc)
{
    if (!cmd || !cmd->waitable)
    {
        return;
    }

    pthread_mutex_lock(&cmd->mutex);
    cmd->rc = rc;
    cmd->done = TRUE;
    pthread_cond_signal(&cmd->cond);
    pthread_mutex_unlock(&cmd->mutex);
}

static int bgp_cmd_wait(bgp_cmd_t *cmd)
{
    if (!cmd || !cmd->waitable)
    {
        return ERRCODE_FAIL;
    }

    pthread_mutex_lock(&cmd->mutex);
    while (!cmd->done)
    {
        pthread_cond_wait(&cmd->cond, &cmd->mutex);
    }
    int rc = cmd->rc;
    pthread_mutex_unlock(&cmd->mutex);
    return rc;
}

static int bgp_cmd_enqueue(bgp_cmd_t *cmd)
{
    /* g_bgp_work_local 可能在 module_cleanup 期间已经 NULL（IPC 派发线程的尾巴上还
     * 可能继续派发到 bgp_worker_post_*）。先 NULL 守门，避免对 NULL->cmd_queue 的解引用。 */
    if (!cmd || !g_bgp_work_local || !g_bgp_work_local->cmd_queue || g_bgp_work_local->cmd_eventfd < 0)
    {
        return -1;
    }

    g_async_queue_push(g_bgp_work_local->cmd_queue, cmd);

    uint64_t one = 1;
    if (write(g_bgp_work_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("BGP: cmd eventfd write failed");
        }
    }

    return 0;
}

void bgp_cmd_drain_queue(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->cmd_queue)
    {
        return;
    }

    bgp_cmd_t *cmd = NULL;
    while ((cmd = (bgp_cmd_t *)g_async_queue_try_pop(g_bgp_work_local->cmd_queue)) != NULL)
    {
        if (cmd->msg)
        {
            dev_ipc_message_free(cmd->msg);
            cmd->msg = NULL;
        }
        if (cmd->type == BGP_CMD_TYPE_APPLY && cmd->apply)
        {
            cmd->apply->rc = BGP_APPLY_RC_FAIL;
            snprintf(cmd->apply->errmsg, sizeof(cmd->apply->errmsg), "BGP server shutdown");
        }
        if (cmd->waitable && !cmd->done)
        {
            bgp_cmd_complete(cmd, ERRCODE_FAIL);
        }
        if (!cmd->waitable)
        {
            bgp_cmd_destroy(cmd);
        }
    }
}

// ============================================================================
// 外部线程投递 API（声明在 bgp_worker.h 保持稳定）
// ============================================================================

int bgp_worker_dispatch_apply(bgp_apply_cmd_t *apply)
{
    if (!apply)
    {
        return -1;
    }
    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_APPLY, NULL, TRUE);
    if (!cmd)
    {
        return -1;
    }
    cmd->apply = apply;
    if (bgp_cmd_enqueue(cmd) != 0)
    {
        bgp_cmd_destroy(cmd);
        return -1;
    }
    bgp_cmd_wait(cmd);
    bgp_cmd_destroy(cmd);
    return 0;
}

int bgp_worker_post_show_cli(dev_ipc_message_t *msg)
{
    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_SHOW_CLI, msg, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_cmd_enqueue(cmd) != 0)
    {
        bgp_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

int bgp_worker_post_route_message(dev_ipc_message_t *msg)
{
    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_ROUTE_MSG, msg, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_cmd_enqueue(cmd) != 0)
    {
        bgp_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

int bgp_worker_post_route_ready(void)
{
    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_ROUTE_READY, NULL, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_cmd_enqueue(cmd) != 0)
    {
        bgp_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

int bgp_worker_post_if_down(void)
{
    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_IF_DOWN, NULL, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_cmd_enqueue(cmd) != 0)
    {
        bgp_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

int bgp_worker_post_vrf_down(void)
{
    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_VRF_DOWN, NULL, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_cmd_enqueue(cmd) != 0)
    {
        bgp_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

int bgp_worker_post_tunnel_message(dev_ipc_message_t *msg)
{
    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_TUNNEL_MSG, msg, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_cmd_enqueue(cmd) != 0)
    {
        bgp_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

int bgp_worker_post_if_event(dev_ipc_message_t *msg)
{
    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_IF_EVENT, msg, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_cmd_enqueue(cmd) != 0)
    {
        bgp_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

int bgp_worker_post_vrf_event(dev_ipc_message_t *msg)
{
    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_VRF_EVENT, msg, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_cmd_enqueue(cmd) != 0)
    {
        bgp_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

void bgp_cmd_post_shutdown(void)
{
    if (!g_bgp_work_local)
    {
        return;
    }

    bgp_cmd_t *cmd = bgp_cmd_create(BGP_CMD_TYPE_SHUTDOWN, NULL, TRUE);
    if (!cmd || bgp_cmd_enqueue(cmd) != 0)
    {
        /* 兜底：投递失败时仍让 worker 线程退出主循环 */
        if (cmd)
        {
            bgp_cmd_destroy(cmd);
        }
        g_bgp_work_local->running = 0;
        return;
    }

    (void)bgp_cmd_wait(cmd);
    bgp_cmd_destroy(cmd);
}

// ============================================================================
// 内部分发（worker 线程执行）
// ============================================================================

static void bgp_cmd_dispatch_apply(bgp_apply_cmd_t *apply)
{
    if (!apply)
    {
        return;
    }
    apply->rc = BGP_APPLY_RC_FAIL;
    apply->errmsg[0] = '\0';

    switch (apply->group_id)
    {
        case BGP_CLI_GROUP_ID_PROTOCOL:
            bgp_cfg_apply_protocol(apply);
            break;
        case BGP_CLI_GROUP_ID_VRF_VIEW:
            bgp_cfg_apply_vrf(apply);
            break;
        case BGP_CLI_GROUP_ID_NEIGHBOR:
            bgp_cfg_apply_neighbor(apply);
            break;
        case BGP_CLI_GROUP_ID_ADDR_FAMILY:
            bgp_cfg_apply_instance(apply);
            break;
        case BGP_CLI_GROUP_ID_AF_NEIGHBOR:
            bgp_cfg_apply_af_neighbor(apply);
            break;
        case BGP_CLI_GROUP_ID_ROUTER_ID:
            bgp_cfg_apply_router_id(apply);
            break;
        case BGP_CLI_GROUP_ID_TIMERS:
            bgp_cfg_apply_timers(apply);
            break;
        case BGP_CLI_GROUP_ID_CONNECT_RETRY:
            bgp_cfg_apply_connect_retry(apply);
            break;
        case BGP_CLI_GROUP_ID_OPEN_CAP:
            bgp_cfg_apply_open_cap(apply);
            break;
        case BGP_CLI_GROUP_ID_IMPORT_ROUTE:
            bgp_cfg_apply_import_route(apply);
            break;
        case BGP_CLI_GROUP_ID_SOURCE_IF:
            bgp_cfg_apply_source_if(apply);
            break;
        case BGP_CLI_GROUP_ID_EBGP_MULTIHOP:
            bgp_cfg_apply_ebgp_multihop(apply);
            break;
        case BGP_CLI_GROUP_ID_QP_ROUTE:
            bgp_cfg_apply_qp_route(apply);
            break;
        case BGP_CLI_GROUP_ID_ROUTE_SELECT:
            bgp_cfg_apply_route_select(apply);
            break;
        case BGP_CLI_GROUP_ID_REFRESH:
            bgp_cfg_apply_refresh(apply);
            break;
        case BGP_CLI_GROUP_ID_CLUSTER_ID:
            bgp_cfg_apply_cluster_id(apply);
            break;
        case BGP_CLI_GROUP_ID_REFLECT_CLIENT:
            bgp_cfg_apply_reflect_client(apply);
            break;
        case BGP_CLI_GROUP_ID_IMPORT_RIB:
            bgp_cfg_apply_import_rib(apply);
            break;
        default:
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Unknown apply group_id %u.", apply->group_id);
            break;
    }
}

static void bgp_cmd_dispatch_show_cli(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case CLI_MSG_TYPE:
            (void)bgp_work_handle_show_msg(msg);
            break;
        case CLI_MSG_TYPE_CONTINUE:
            (void)bgp_work_handle_continue_msg(msg);
            break;
        default:
            LOG_WARN("BGP: unsupported show CLI message type=0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

static void bgp_cmd_dispatch_route_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case ROUTE_MSG_TYPE_UPDATE:
        {
            uint32_t imported = bgp_import_route_on_update(msg);
            if (imported > 0)
            {
                LOG_DEBUG("BGP: ROUTE_UPDATE imported=%u", imported);
            }
            break;
        }

        case ROUTE_MSG_TYPE_REPORT:
        {
            uint32_t imported = bgp_import_route_on_report(msg);
            LOG_INFO("BGP: ROUTE_REPORT entries imported=%u", imported);
            break;
        }

        case ROUTE_MSG_TYPE_NH_NOTIFY:
        {
            if (!msg->payload || msg->payload_len < sizeof(route_nh_iter_notify_t))
            {
                LOG_WARN("BGP: ROUTE_NH_NOTIFY payload too short: %u", msg->payload_len);
                break;
            }
            uint32_t touched = bgp_relay_handle_nh_notify((const route_nh_iter_notify_t *)msg->payload);
            if (touched > 0)
            {
                LOG_DEBUG("BGP: ROUTE_NH_NOTIFY touched=%u", touched);
            }
            break;
        }

        default:
            LOG_WARN("BGP: unsupported route message type=0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

static void bgp_cmd_dispatch_tunnel_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case TUNNEL_MSG_TYPE_RESOLVE_NOTIFY:
        {
            if (!msg->payload || msg->payload_len < sizeof(tunnel_resolve_notify_t))
            {
                LOG_WARN("BGP: TUNNEL_RESOLVE_NOTIFY payload too short: %u", msg->payload_len);
                break;
            }
            uint32_t touched = bgp_relay_handle_tunnel_notify((const tunnel_resolve_notify_t *)msg->payload);
            if (touched > 0)
            {
                LOG_DEBUG("BGP: TUNNEL_RESOLVE_NOTIFY touched=%u", touched);
            }
            break;
        }
        default:
            LOG_WARN("BGP: unsupported tunnel message type=0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// 主派发入口（worker 主循环调用）
// ============================================================================

gboolean bgp_cmd_process_event(void)
{
    if (!g_bgp_work_local || g_bgp_work_local->cmd_eventfd < 0 || !g_bgp_work_local->cmd_queue)
    {
        return FALSE;
    }

    uint64_t v;
    while (read(g_bgp_work_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
        /* drain eventfd counter */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("BGP: cmd eventfd read failed");
    }

    gboolean stop = FALSE;
    bgp_cmd_t *cmd = NULL;
    while ((cmd = (bgp_cmd_t *)g_async_queue_try_pop(g_bgp_work_local->cmd_queue)) != NULL)
    {
        switch (cmd->type)
        {
            case BGP_CMD_TYPE_SHOW_CLI:
                bgp_cmd_dispatch_show_cli(cmd->msg);
                cmd->msg = NULL;
                break;

            case BGP_CMD_TYPE_ROUTE_MSG:
                bgp_cmd_dispatch_route_msg(cmd->msg);
                cmd->msg = NULL;
                break;

            case BGP_CMD_TYPE_ROUTE_READY:
                if (dev_ipc_wait_connected(bgp_local_ipc_ctx(), DEV_MODULE_ID_ROUTE, DEV_IPC_WAIT_PEER_MS) !=
                    ERRCODE_SUCCESS)
                {
                    LOG_WARN("BGP: ROUTE connection not ready in time; replay deferred to next READY");
                    break;
                }
                bgp_import_route_resubscribe_protocol_imports(g_bgp_work_local->protocol);
                /* 先把本模块的 nexthop 对象按原 id 反刷到 ROUTE（重建对象），
                 * 再重注册迭代 watch（沿用同一 id）并重下刷路由 */
                bgp_nexthop_resync_all(g_bgp_work_local->protocol);
                bgp_relay_reregister_route_nexthops();
                bgp_route_flush_replay_flushed_all();
                break;

            case BGP_CMD_TYPE_TUNNEL_MSG:
                bgp_cmd_dispatch_tunnel_msg(cmd->msg);
                cmd->msg = NULL;
                break;

            case BGP_CMD_TYPE_APPLY:
                bgp_cmd_dispatch_apply(cmd->apply);
                break;

            case BGP_CMD_TYPE_IF_EVENT:
                if_api_cache_on_event(cmd->msg);
                if (cmd->msg)
                {
                    dev_ipc_message_free(cmd->msg);
                    cmd->msg = NULL;
                }
                break;

            case BGP_CMD_TYPE_IF_DOWN:
            {
                /* IF 模块下线：
                 *   1) 清掉 IF 共享缓存，避免后续解析读到陈旧 source-addr/ifindex。
                 *   2) 对所有配置了 source-interface 的会话调 bgp_neighbor_down——
                 *      它会发送 NOTIFICATION Cease/Admin-Reset、取消所有定时器、关闭
                 *      TCP、flush peer 路由、purge session 路由、FSM 重置到 ACTIVE。
                 * 路由 nexthop 可达性不在此重注册：ROUTE 进程仍存活，watch 仍在；
                 * ISIS/静态路由等撤销或 IF 事件触发 route_recompute_iter_paths() 后，
                 * ROUTE 会通过 ROUTE_MSG_TYPE_NH_NOTIFY 通知 BGP（bgp_relay_handle_nh_notify）。
                 * bgp_relay_reregister_route_nexthops 仅用于 ROUTE 进程重启后 watch 丢失。
                 * 没配 source-interface 的会话依赖内核选源，TCP 不会立刻断；它们走
                 * 原有 BGP hold-time 机制由对端拆链。 */
                LOG_INFO("BGP: IF DOWN detected, flushing IF cache + tearing source-if bound sessions");
                if_api_cache_cleanup();
                if_api_cache_init();
                if (g_bgp_work_local && g_bgp_work_local->protocol && g_bgp_work_local->protocol->vrf_hash)
                {
                    GHashTableIter vit;
                    gpointer vk, vv;
                    g_hash_table_iter_init(&vit, g_bgp_work_local->protocol->vrf_hash);
                    while (g_hash_table_iter_next(&vit, &vk, &vv))
                    {
                        bgp_vrf_t *vrf = (bgp_vrf_t *)vv;
                        if (!vrf || !vrf->sess_hash)
                        {
                            continue;
                        }
                        /* 复制 session 指针到临时数组：bgp_neighbor_down 可能间接修改
                         * sess_hash（虽然当前实现不会，但要保稳）。 */
                        GPtrArray *snapshot = g_ptr_array_new();
                        GHashTableIter sit;
                        gpointer sk, sv;
                        g_hash_table_iter_init(&sit, vrf->sess_hash);
                        while (g_hash_table_iter_next(&sit, &sk, &sv))
                        {
                            bgp_session_t *sess = (bgp_session_t *)sv;
                            if (sess && sess->source_if_name[0] != '\0')
                            {
                                g_ptr_array_add(snapshot, sess);
                            }
                        }
                        for (guint i = 0; i < snapshot->len; i++)
                        {
                            bgp_neighbor_down((bgp_session_t *)snapshot->pdata[i], g_bgp_work_local->epoll_fd);
                        }
                        /* g_ptr_array_new 没设 element_free_func，free_seg=TRUE 仅释放
                         * 内部 pdata 段，不会触动借用的 session 指针，所有权仍归 sess_hash */
                        g_ptr_array_free(snapshot, TRUE);
                    }
                }
                break;
            }

            case BGP_CMD_TYPE_VRF_EVENT:
                /* BGP 业务清理已在 VRF DOWN 路径完成；此处只走 lib 缓存维护与
                 * BGP 内部 VRF 事件联动。 */
                vrf_api_cache_on_event(cmd->msg);
                bgp_apply_vrf_event(cmd->msg);
                if (cmd->msg)
                {
                    dev_ipc_message_free(cmd->msg);
                    cmd->msg = NULL;
                }
                break;

            case BGP_CMD_TYPE_VRF_DOWN:
                /* VRF 模块 DOWN：拆所有非 public 的 bgp_vrf_t（级联 session/instance/neighbor），
                 * 再清掉 vrf_api cache。DB 保留，等 SMOOTHEND 后再恢复。 */
                bgp_apply_vrf_purge_non_public();
                vrf_api_cache_clear();
                bgp_vrf_import_purge_all(); /* IRT 索引整体清空，待 REPLAY 的 IMPORT_RT_ADD 重建 */
                break;

            case BGP_CMD_TYPE_SHUTDOWN:
                g_bgp_work_local->running = 0;
                stop = TRUE;
                break;

            default:
                break;
        }

        if (cmd->waitable)
        {
            bgp_cmd_complete(cmd, ERRCODE_SUCCESS);
        }
        else
        {
            bgp_cmd_destroy(cmd);
        }

        if (stop)
        {
            break;
        }
    }

    return stop;
}
