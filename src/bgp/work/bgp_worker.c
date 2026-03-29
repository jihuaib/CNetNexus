/**
 * @file   bgp_worker.c
 * @brief  BGP worker 线程：epoll 事件循环、连接管理与命令队列
 */
#include "bgp_worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bgp_bmp.h"
#include "bgp_bmp_cfg_apply.h"
#include "bgp_bmp_cli.h"
#include "bgp_calc.h"
#include "bgp_cfg_apply.h"
#include "bgp_cli.h"
#include "bgp_conn.h"
#include "bgp_fsm.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_relay.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_work.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

/** BGP server epoll 单次最大事件数 */
#define BGP_MAX_EPOLL_EVENTS 16

/** BGP 协议标准端口 */
#define BGP_LISTEN_PORT 179

/** epoll data.ptr sentinel：区分 listen fd 事件与连接 fd 事件 */
static char bgp_listen_tag;
/** epoll data.ptr sentinel：区分 worker->server 命令事件 */
static char bgp_cmd_tag;

bgp_work_local_t *g_bgp_work_local = NULL;

static void bgp_worker_runtime_cleanup(void);

// ============================================================================
// 命令队列（worker -> server）
// ============================================================================

typedef enum bgp_worker_cmd_type
{
    BGP_WORKER_CMD_TYPE_SHOW_CLI = 1, /**< show CLI 命令派发（CLI_MSG_TYPE/CLI_MSG_TYPE_CONTINUE） */
    BGP_WORKER_CMD_TYPE_SHUTDOWN = 2,
    BGP_WORKER_CMD_TYPE_APPLY = 3,     /**< 跨线程配置应用命令 */
    BGP_WORKER_CMD_TYPE_ROUTE_MSG = 4, /**< ROUTE_MSG_TYPE_UPDATE/REPORT/NH_NOTIFY */
} bgp_worker_cmd_type_t;

typedef struct bgp_server_cmd
{
    bgp_worker_cmd_type_t type;
    dev_ipc_message_t *msg; /**< BGP_WORKER_CMD_TYPE_SHOW_CLI / BGP_WORKER_CMD_TYPE_ROUTE_MSG */
    bgp_apply_cmd_t *apply; /**< BGP_WORKER_CMD_TYPE_APPLY（借用引用，不持有所有权） */
    gboolean waitable;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    gboolean done;
    int rc;
} bgp_worker_cmd_t;

static bgp_worker_cmd_t *bgp_worker_cmd_create(bgp_worker_cmd_type_t type, dev_ipc_message_t *msg, gboolean waitable)
{
    bgp_worker_cmd_t *cmd = g_malloc0(sizeof(*cmd));
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

static void bgp_worker_cmd_destroy(bgp_worker_cmd_t *cmd)
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

static void bgp_worker_cmd_complete(bgp_worker_cmd_t *cmd, int rc)
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

static int bgp_worker_cmd_wait(bgp_worker_cmd_t *cmd)
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

static int bgp_worker_cmd_enqueue(bgp_worker_cmd_t *cmd)
{
    if (!cmd || !g_bgp_work_local->cmd_queue || g_bgp_work_local->cmd_eventfd < 0)
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

static void bgp_worker_cmd_drain_queue(void)
{
    if (!g_bgp_work_local->cmd_queue)
    {
        return;
    }

    bgp_worker_cmd_t *cmd = NULL;
    while ((cmd = (bgp_worker_cmd_t *)g_async_queue_try_pop(g_bgp_work_local->cmd_queue)) != NULL)
    {
        if (cmd->msg)
        {
            dev_ipc_message_free(cmd->msg);
            cmd->msg = NULL;
        }
        if (cmd->type == BGP_WORKER_CMD_TYPE_APPLY && cmd->apply)
        {
            cmd->apply->rc = BGP_APPLY_RC_FAIL;
            snprintf(cmd->apply->errmsg, sizeof(cmd->apply->errmsg), "BGP server shutdown");
        }
        if (cmd->waitable && !cmd->done)
        {
            bgp_worker_cmd_complete(cmd, ERRCODE_FAIL);
        }
        if (!cmd->waitable)
        {
            bgp_worker_cmd_destroy(cmd);
        }
    }
}

// ============================================================================
// 配置应用命令派发（server 线程执行）
// ============================================================================

/**
 * @brief 重置 VRF 内所有有活跃连接的 session（router-id / timer 变更后调用）
 */
void bgp_server_reset_all_sessions(bgp_vrf_t *vrf)
{
    if (!vrf || !vrf->sess_hash)
    {
        return;
    }
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, vrf->sess_hash);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        bgp_session_t *sess = (bgp_session_t *)value;
        if (sess && (sess->pri_conn || sess->sec_conn))
        {
            bgp_neighbor_down(sess, g_bgp_work_local->epoll_fd);
        }
    }
}

/**
 * @brief 按当前 VRF connect-retry 配置，重排已挂起的 retry 定时器
 *
 * 用于处理 connect-retry 参数动态修改：已存在的 retry timer 需要立即生效新间隔。
 */
void bgp_server_rearm_retry_timers(bgp_vrf_t *vrf)
{
    if (!vrf || !vrf->sess_hash)
    {
        return;
    }

    uint16_t retry_sec = (vrf->connect_retry > 0) ? vrf->connect_retry : BGP_TIMER_DEFAULT_CONNECT_RETRY;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, vrf->sess_hash);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        bgp_session_t *sess = (bgp_session_t *)value;
        if (!sess || sess->retry_timerfd < 0)
        {
            continue;
        }
        bgp_session_cancel_retry(sess, g_bgp_work_local->epoll_fd);
        bgp_session_arm_retry(sess, g_bgp_work_local->epoll_fd, retry_sec);
    }
}

/**
 * @brief 在 server 线程中执行结构化配置应用命令
 *
 * 根据 group_id 分发到 bgp_cfg_apply_* 对应函数，由各函数完成校验、短路和内存操作。
 */
static void bgp_worker_dispatch_apply_cmd(bgp_apply_cmd_t *apply)
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
        case BGP_CLI_GROUP_ID_BMP_INSTANCE:
            bgp_bmp_cfg_apply_instance(apply);
            break;
        case BGP_CLI_GROUP_ID_BMP_COLLECTOR:
            bgp_bmp_cfg_apply_collector(apply);
            break;
        case BGP_CLI_GROUP_ID_BMP_STATS_INTERVAL:
            bgp_bmp_cfg_apply_stats(apply);
            break;
        case BGP_CLI_GROUP_ID_BMP_RECONNECT:
            bgp_bmp_cfg_apply_reconnect(apply);
            break;
        case BGP_CLI_GROUP_ID_BMP_MONITOR:
            bgp_bmp_cfg_apply_monitor(apply);
            break;
        default:
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Unknown apply group_id %u.", apply->group_id);
            break;
    }
}

void bgp_worker_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd,
                                   bgp_peer_update_ingest_stats_t *stats)
{
    bgp_relay_ingest_peer_update(session, upd, stats);
}

void bgp_worker_flush_peer_routes(uint32_t vrf_id, const net_addr_t *source)
{
    bgp_relay_flush_peer_routes(vrf_id, source);
}

/**
 * @brief 将一条 ROUTE 路由条目导入（或撤销）到 BGP RIB
 * @return 1=已处理，0=被过滤/忽略
 */
static int bgp_import_route_entry(const route_msg_entry_t *entry)
{
    if (!entry)
    {
        return 0;
    }

    if (!g_bgp_work_local->protocol)
    {
        return 0;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_work_local->protocol, entry->vrf_id);
    if (!vrf)
    {
        return 0;
    }

    /* 查找对应 AFI/SAFI 实例（不自动创建：未配置 AF 则忽略） */
    bgp_afi_t afi = (bgp_afi_t)entry->afi;
    bgp_safi_t safi = (entry->safi == 0) ? BGP_SAFI_UNICAST : (bgp_safi_t)entry->safi;
    if ((afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6) || safi != BGP_SAFI_UNICAST)
    {
        return 0;
    }
    sa_family_t expected_family = (afi == BGP_AFI_IPV4) ? AF_INET : AF_INET6;
    uint8_t max_prefix_len = (afi == BGP_AFI_IPV4) ? 32u : 128u;
    if (entry->prefix_addr.family != expected_family || entry->source_addr.family != expected_family ||
        entry->nexthop_addr.family != expected_family || entry->prefix_len > max_prefix_len)
    {
        return 0;
    }

    bgp_instance_t *inst = (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi));
    if (!inst)
    {
        return 0;
    }

    if (!(inst->import_protos & (1u << entry->protocol)))
    {
        /* import-route 路径遵循 import-route 协议开关 */
        return 0;
    }

    /* 构建 NLRI entry（直接使用二进制地址，无字符串转换） */
    bgp_nlri_entry_t nlri;
    memset(&nlri, 0, sizeof(nlri));
    nlri.afi = (uint16_t)afi;
    nlri.safi = (uint8_t)safi;
    nlri.type = BGP_NLRI_PREFIX;
    nlri.prefix.prefix.prefix_len = entry->prefix_len;
    nlri.prefix.prefix.addr = entry->prefix_addr;
    nlri.prefix.has_rd = false;
    nlri.prefix.has_label = false;

    net_addr_t src = entry->source_addr;

    if (entry->is_withdraw)
    {
        int rc = bgp_rib_unreach_one(inst->rib, &nlri, &src);
        /* 与对端 UPDATE 处理保持一致：撤销成功后触发一次优选，决定是否发 WITHDRAW */
        if (rc == 1 && inst->calc_queue)
        {
            bgp_calc_queue_push(inst->calc_queue, inst, &nlri);
        }
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(&nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: Import route withdraw %s", nlri_str);
    }
    else
    {
        /* import-route 路径：构建合成 BGP 属性（ORIGIN=INCOMPLETE，AS_PATH 为空） */
        bgp_attr_t attr;
        memset(&attr, 0, sizeof(attr));
        attr.origin = BGP_ORIGIN_INCOMPLETE;
        attr.local_pref = 100;
        attr.has_local_pref = true;

        bgp_nexthop_t nexthop;
        memset(&nexthop, 0, sizeof(nexthop));
        nexthop.has_link_local = false;
        nexthop.global = entry->nexthop_addr;

        bgp_rthead_t *head = bgp_rib_ensure_head(inst->rib, &nlri);
        if (!head)
        {
            return 0;
        }

        bgp_route_node_t *route = bgp_rthead_lookup_route_mut(head, &src);
        int rc = 0;
        if (!route)
        {
            route = bgp_rthead_create_route(inst->rib, head, &src);
            if (!route)
            {
                return 0;
            }
            rc = 1;
        }
        if (bgp_rib_route_apply_reach(route, (uint32_t)entry->protocol, &attr, &nexthop) != 0)
        {
            return 0;
        }
        if (rc >= 0 && inst->calc_queue)
        {
            bgp_calc_queue_push(inst->calc_queue, inst, &nlri);
        }

        char nh_str[64], src_str[64];
        net_addr_to_str(&entry->nexthop_addr, nh_str, sizeof(nh_str));
        net_addr_to_str(&entry->source_addr, src_str, sizeof(src_str));
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(&nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: Import route add %s nh=%s src=%s", nlri_str, nh_str, src_str);
    }

    return 1;
}

/**
 * @brief 批量导入 ROUTE 路由条目
 * @return 实际导入条目数
 */
static uint32_t bgp_handle_route_entries(const route_msg_entry_t *entries, uint32_t route_count)
{
    if (!entries || route_count == 0)
    {
        return 0;
    }

    uint32_t imported = 0;
    for (uint32_t i = 0; i < route_count; ++i)
    {
        imported += (uint32_t)bgp_import_route_entry(&entries[i]);
    }
    return imported;
}

/**
 * @brief 处理 ROUTE 模块推送的增量路由更新（ROUTE_MSG_TYPE_UPDATE）
 * @return 实际导入条目数
 */
static uint32_t bgp_handle_route_update(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(route_msg_entry_t))
    {
        LOG_WARN("BGP: ROUTE_UPDATE payload too short: %u bytes", msg ? msg->payload_len : 0u);
        return 0;
    }

    const route_msg_entry_t *entry = (const route_msg_entry_t *)msg->payload;
    return bgp_handle_route_entries(entry, 1);
}

/**
 * @brief 处理 ROUTE 模块全量路由快照（ROUTE_MSG_TYPE_REPORT）
 * @return 实际导入条目数
 */
static uint32_t bgp_handle_route_report(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(route_msg_report_t))
    {
        LOG_WARN("BGP: ROUTE_REPORT payload too short: %u bytes", msg ? msg->payload_len : 0u);
        return 0;
    }

    const route_msg_report_t *report = (const route_msg_report_t *)msg->payload;
    size_t routes_bytes = (size_t)msg->payload_len - sizeof(route_msg_report_t);
    uint32_t max_routes = (uint32_t)(routes_bytes / sizeof(route_msg_entry_t));
    if (report->route_count > max_routes)
    {
        LOG_WARN("BGP: ROUTE_REPORT malformed: payload=%u route_count=%u max=%u", msg->payload_len, report->route_count,
                 max_routes);
        return 0;
    }

    return bgp_handle_route_entries(report->routes, report->route_count);
}

int bgp_worker_dispatch_apply(bgp_apply_cmd_t *apply)
{
    if (!apply)
    {
        return -1;
    }
    bgp_worker_cmd_t *cmd = bgp_worker_cmd_create(BGP_WORKER_CMD_TYPE_APPLY, NULL, TRUE);
    if (!cmd)
    {
        return -1;
    }
    cmd->apply = apply;
    if (bgp_worker_cmd_enqueue(cmd) != 0)
    {
        bgp_worker_cmd_destroy(cmd);
        return -1;
    }
    bgp_worker_cmd_wait(cmd);
    bgp_worker_cmd_destroy(cmd);
    return 0;
}

int bgp_worker_post_show_cli(dev_ipc_message_t *msg)
{
    bgp_worker_cmd_t *cmd = bgp_worker_cmd_create(BGP_WORKER_CMD_TYPE_SHOW_CLI, msg, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_worker_cmd_enqueue(cmd) != 0)
    {
        bgp_worker_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

int bgp_worker_post_route_message(dev_ipc_message_t *msg)
{
    bgp_worker_cmd_t *cmd = bgp_worker_cmd_create(BGP_WORKER_CMD_TYPE_ROUTE_MSG, msg, FALSE);
    if (!cmd)
    {
        return -1;
    }

    if (bgp_worker_cmd_enqueue(cmd) != 0)
    {
        bgp_worker_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

static void bgp_worker_dispatch_show_cli_cmd(dev_ipc_message_t *msg)
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

static void bgp_worker_dispatch_route_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case ROUTE_MSG_TYPE_UPDATE:
        {
            uint32_t imported = bgp_handle_route_update(msg);
            if (imported > 0)
            {
                LOG_DEBUG("BGP: ROUTE_UPDATE imported=%u", imported);
            }
            break;
        }

        case ROUTE_MSG_TYPE_REPORT:
        {
            uint32_t imported = bgp_handle_route_report(msg);
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

static gboolean bgp_process_cmd_event(void)
{
    if (g_bgp_work_local->cmd_eventfd < 0 || !g_bgp_work_local->cmd_queue)
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
    bgp_worker_cmd_t *cmd = NULL;
    while ((cmd = (bgp_worker_cmd_t *)g_async_queue_try_pop(g_bgp_work_local->cmd_queue)) != NULL)
    {
        switch (cmd->type)
        {
            case BGP_WORKER_CMD_TYPE_SHOW_CLI:
                bgp_worker_dispatch_show_cli_cmd(cmd->msg);
                cmd->msg = NULL;
                break;

            case BGP_WORKER_CMD_TYPE_ROUTE_MSG:
                bgp_worker_dispatch_route_msg(cmd->msg);
                cmd->msg = NULL;
                break;

            case BGP_WORKER_CMD_TYPE_APPLY:
                bgp_worker_dispatch_apply_cmd(cmd->apply);
                break;

            case BGP_WORKER_CMD_TYPE_SHUTDOWN:
                g_bgp_work_local->running = 0;
                stop = TRUE;
                break;

            default:
                break;
        }

        if (cmd->waitable)
        {
            bgp_worker_cmd_complete(cmd, ERRCODE_SUCCESS);
        }
        else
        {
            bgp_worker_cmd_destroy(cmd);
        }

        if (stop)
        {
            break;
        }
    }

    return stop;
}

// ============================================================================
// BGP listen socket 管理
// ============================================================================

void bgp_listen_start(void)
{
    if (g_bgp_work_local->epoll_fd < 0)
    {
        return;
    }
    if (g_bgp_work_local->listen_fd >= 0)
    {
        return; /* 已在监听，幂等 */
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_PERROR("BGP: Failed to create listen socket");
        return;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(BGP_LISTEN_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_PERROR("BGP: bind 0.0.0.0:179 failed");
        close(fd);
        return;
    }

    if (listen(fd, 32) < 0)
    {
        LOG_PERROR("BGP: listen failed");
        close(fd);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = &bgp_listen_tag;
    if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD listen fd failed");
        close(fd);
        return;
    }

    g_bgp_work_local->listen_fd = fd;
    LOG_INFO("BGP: Listening on 0.0.0.0:179 (fd=%d)", fd);
}

void bgp_listen_stop(void)
{
    if (g_bgp_work_local->listen_fd < 0)
    {
        return;
    }
    if (g_bgp_work_local->epoll_fd >= 0)
    {
        epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_work_local->listen_fd, NULL);
    }
    close(g_bgp_work_local->listen_fd);
    g_bgp_work_local->listen_fd = -1;
    LOG_INFO("BGP: Stopped listening on 0.0.0.0:179");
}

// ============================================================================
// BGP server 辅助函数
// ============================================================================

/**
 * @brief 从 epoll 移除、销毁连接对象，并将 session 槽位置 NULL
 * @param slot &sess->pri_conn 或 &sess->sec_conn
 */
static void bgp_conn_close(bgp_session_t *sess, bgp_conn_t **slot)
{
    if (!slot || !*slot)
    {
        return;
    }
    bgp_conn_t *conn = *slot;
    if (sess)
    {
        if (slot == &sess->pri_conn)
        {
            sess->pri_last_socket_error = conn->last_socket_error;
        }
        else if (slot == &sess->sec_conn)
        {
            sess->sec_last_socket_error = conn->last_socket_error;
        }
    }
    if (conn->fd >= 0)
    {
        epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    }
    bgp_conn_destroy(conn);
    *slot = NULL;
}

/**
 * @brief 将 sec_conn 提升为 pri_conn（pri_conn 必须已为 NULL）
 */
static void bgp_session_promote_sec(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP: Passive connection fd=%d promoted to pri_conn (neighbor=%s)", sess->sec_conn->fd, addr_str);
    sess->pri_conn = sess->sec_conn;
    sess->sec_conn = NULL;
    sess->pri_last_socket_error = sess->pri_conn->last_socket_error;
    sess->sec_last_socket_error = 0;
}

// ============================================================================
// BGP server 线程 — 事件处理函数
// ============================================================================

/**
 * @brief 处理全局 listener 上的被动入站连接
 */
static void bgp_handle_passive_accept(void)
{
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    struct sockaddr_storage peer_sa;
    socklen_t addr_len = sizeof(peer_sa);
    int conn_fd = accept(g_bgp_work_local->listen_fd, (struct sockaddr *)&peer_sa, &addr_len);

    if (conn_fd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("BGP: accept failed");
        }
        return;
    }

    /* 解析来源地址 */
    net_addr_t from_addr;
    memset(&from_addr, 0, sizeof(from_addr));
    char from_ip[64] = "";

    if (peer_sa.ss_family == AF_INET)
    {
        struct sockaddr_in *sa4 = (struct sockaddr_in *)&peer_sa;
        from_addr.family = AF_INET;
        memcpy(&from_addr.u.v4, &sa4->sin_addr, sizeof(sa4->sin_addr));
        inet_ntop(AF_INET, &sa4->sin_addr, from_ip, sizeof(from_ip));
    }
    else if (peer_sa.ss_family == AF_INET6)
    {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&peer_sa;
        from_addr.family = AF_INET6;
        memcpy(&from_addr.u.v6, &sa6->sin6_addr, sizeof(sa6->sin6_addr));
        inet_ntop(AF_INET6, &sa6->sin6_addr, from_ip, sizeof(from_ip));
    }
    else
    {
        LOG_WARN("BGP: Rejecting connection with unknown address family");
        close(conn_fd);
        return;
    }

    if (!proto)
    {
        LOG_WARN("BGP: Protocol not initialized, rejecting connection from %s", from_ip);
        close(conn_fd);
        return;
    }

    bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
    bgp_session_t *sess = vrf0 ? bgp_vrf_find_session(vrf0, &from_addr) : NULL;
    if (!sess || !bgp_vrf_neighbor_has_any_af(vrf0, &from_addr))
    {
        LOG_WARN("BGP: Rejecting connection from %s (no AF neighbor configured)", from_ip);
        close(conn_fd);
        return;
    }

    if (sess->sec_conn)
    {
        LOG_WARN("BGP: Rejecting connection from %s (sec_conn already exists fd=%d)", from_ip, sess->sec_conn->fd);
        close(conn_fd);
        return;
    }

    if (sess->pri_conn && !sess->pri_conn->is_connecting && sess->pri_conn->state == BGP_CONN_STATE_ESTABLISHED)
    {
        LOG_INFO("BGP: neighbor %s session established (fd=%d), rejecting new passive connection fd=%d", from_ip,
                 sess->pri_conn->fd, conn_fd);
        close(conn_fd);
        return;
    }

    LOG_INFO("BGP: neighbor %s passive TCP connection (fd=%d)", from_ip, conn_fd);

    bgp_conn_t *conn = bgp_conn_create(sess);
    conn->fd = conn_fd;
    conn->is_active = FALSE;
    conn->is_connecting = FALSE;
    memcpy(&conn->peer_addr, &from_addr, sizeof(from_addr));
    /* conn->state 已由 bgp_conn_create 初始化为 BGP_CONN_STATE_OPEN_SENT */

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD passive connection failed");
        bgp_conn_destroy(conn);
        return;
    }

    if (sess->pri_conn)
    {
        if (sess->pri_conn->is_connecting)
        {
            LOG_INFO("BGP: neighbor %s passive connection fd=%d (active fd=%d still TCP handshaking, §6.8 pending)",
                     from_ip, conn_fd, sess->pri_conn->fd);
        }
        else
        {
            LOG_INFO("BGP: neighbor %s passive connection fd=%d (active fd=%d in OPEN negotiation, §6.8 pending)",
                     from_ip, conn_fd, sess->pri_conn->fd);
        }
        sess->sec_conn = conn;
        sess->sec_last_socket_error = 0;
    }
    else
    {
        sess->pri_conn = conn;
        sess->pri_last_socket_error = 0;
    }

    /* 触发 FSM 事件：发送 OPEN 并迁移状态 */
    bgp_fsm_event(sess, conn, BGP_EVT_TCP_CONNECTION_CONFIRMED, g_bgp_work_local->epoll_fd);
}

/**
 * @brief 处理主动连接完成事件（EPOLLOUT）
 */
static void bgp_handle_active_connect(bgp_conn_t *conn)
{
    bgp_session_t *sess = conn->session;

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len);
    conn->last_socket_error = err;

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    if (err != 0)
    {
        LOG_WARN("BGP: Active connection to %s failed: %s (errno=%d, fd=%d)", addr_str, strerror(err), err, conn->fd);
        bgp_fsm_event(sess, conn, BGP_EVT_TCP_CONNECTION_FAILS, g_bgp_work_local->epoll_fd);
        return;
    }

    conn->last_socket_error = 0;

    if (sess->sec_conn)
    {
        LOG_INFO("BGP: Active TCP to %s established (fd=%d), sec_conn fd=%d also present, §6.8 collision pending",
                 addr_str, conn->fd, sess->sec_conn->fd);
    }
    else
    {
        LOG_INFO("BGP: Active TCP connection to %s established (fd=%d)", addr_str, conn->fd);
    }

    /* 将 epoll 改为 EPOLLIN（接收 BGP 报文），清除连接中标志 */
    conn->is_connecting = FALSE;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);

    bgp_fsm_event(sess, conn, BGP_EVT_TCP_CR_ACKED, g_bgp_work_local->epoll_fd);
}

/**
 * @brief 处理已建立连接上的 BGP 数据（EPOLLIN）
 */
static void bgp_handle_data(bgp_conn_t *conn)
{
    bgp_session_t *sess = conn->session;
    bgp_conn_state_t old_conn_state = conn->state;
    int epoll_fd = g_bgp_work_local->epoll_fd;

    int ret = bgp_pkt_on_data(conn);

    if (ret == BGP_PKT_ON_DATA_COLLISION_CLOSE_ME)
    {
        /* §6.8 碰撞检测：当前连接为败方，通知 FSM 关闭 */
        bgp_fsm_event(sess, conn, BGP_EVT_OPEN_COLLISION_DUMP, epoll_fd);
        return;
    }

    if (ret == BGP_PKT_ON_DATA_COLLISION_CLOSE_OTHER)
    {
        /* §6.8 碰撞检测：当前连接为胜方，直接关闭另一条连接 */
        bgp_conn_t **slot = (sess->pri_conn == conn) ? &sess->pri_conn : &sess->sec_conn;
        bgp_conn_t **other_slot = (slot == &sess->pri_conn) ? &sess->sec_conn : &sess->pri_conn;
        if (*other_slot)
        {
            char addr_str[64];
            net_addr_to_str(&(*other_slot)->peer_addr, addr_str, sizeof(addr_str));
            LOG_INFO("BGP: §6.8 collision: closing %s connection (fd=%d) with %s",
                     (*other_slot)->is_active ? "active" : "passive", (*other_slot)->fd, addr_str);
            bgp_conn_close(sess, other_slot);
        }
        if (!sess->pri_conn && sess->sec_conn)
        {
            bgp_session_promote_sec(sess);
        }
        /* 胜方继续：由下方状态变化检测触发 BGP_OPEN 事件 */
    }
    else if (ret < 0)
    {
        /* TCP 断开或协议错误：通知 FSM 关闭连接并调度重连 */
        bgp_fsm_event(sess, conn, BGP_EVT_TCP_CONNECTION_FAILS, epoll_fd);
        return;
    }

    /* 根据 conn->state 变化触发对应 FSM 事件 */
    if (old_conn_state == BGP_CONN_STATE_OPEN_SENT && conn->state == BGP_CONN_STATE_OPEN_CONFIRM)
    {
        /* bgp_pkt_on_data 已发 KEEPALIVE 并将 conn->state 置 OPEN_CONFIRM */
        bgp_fsm_event(sess, conn, BGP_EVT_BGP_OPEN, epoll_fd);
    }
    else if (old_conn_state == BGP_CONN_STATE_OPEN_CONFIRM && conn->state == BGP_CONN_STATE_ESTABLISHED)
    {
        /* bgp_pkt_on_data 已将 conn->state 置 ESTABLISHED */
        bgp_fsm_event(sess, conn, BGP_EVT_KEEPALIVE_MSG, epoll_fd);
    }
    else if (conn->state == BGP_CONN_STATE_ESTABLISHED && sess->hold_reset_pending)
    {
        /* KEEPALIVE/UPDATE in Established：直接重置 hold 定时器（FSM 表中为 NULL 条目） */
        sess->hold_reset_pending = FALSE;
        bgp_session_reset_hold(sess);
    }
}

static void bgp_handle_ka_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->ka_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read ka timerfd");
    }
    bgp_fsm_event(sess, sess->pri_conn, BGP_EVT_KEEPALIVE_TIMER_EXPIRED, g_bgp_work_local->epoll_fd);
}

static void bgp_handle_hold_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->hold_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read hold timerfd");
    }
    bgp_fsm_event(sess, sess->pri_conn, BGP_EVT_HOLD_TIMER_EXPIRED, g_bgp_work_local->epoll_fd);
}

static void bgp_handle_retry_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->retry_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read timerfd");
    }

    /* 守卫：若邻居 AF 已删除，取消定时器并退出 */
    bgp_protocol_t *proto = g_bgp_work_local->protocol;
    bgp_vrf_t *vrf0 = proto ? bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID) : NULL;
    if (!vrf0 || !bgp_vrf_neighbor_has_any_af(vrf0, &sess->neighbor_addr))
    {
        bgp_session_cancel_retry(sess, g_bgp_work_local->epoll_fd);
        return;
    }

    bgp_fsm_event(sess, NULL, BGP_EVT_CONNECT_RETRY_EXPIRED, g_bgp_work_local->epoll_fd);
}

// ============================================================================
// BGP server 线程
// ============================================================================

static void *bgp_worker_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "bgp-worker");
    log_set_tag("bgp");

    struct epoll_event events[BGP_MAX_EPOLL_EVENTS];

    while (g_bgp_work_local->running)
    {
        int nfds = epoll_wait(g_bgp_work_local->epoll_fd, events, BGP_MAX_EPOLL_EVENTS, 1000);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("BGP: epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            uintptr_t raw = (uintptr_t)events[i].data.ptr;

            if (events[i].data.ptr == (void *)&bgp_listen_tag)
            {
                bgp_handle_passive_accept();
                continue;
            }
            if (events[i].data.ptr == (void *)&bgp_cmd_tag)
            {
                if (bgp_process_cmd_event())
                {
                    break;
                }
                continue;
            }

            if (raw & 1UL)
            {
                bgp_timer_sentinel_t *sentinel = (bgp_timer_sentinel_t *)(raw & ~1UL);
                switch (sentinel->type)
                {
                    case BGP_TIMER_TYPE_RETRY:
                        bgp_handle_retry_timer(sentinel->session);
                        break;
                    case BGP_TIMER_TYPE_KEEPALIVE:
                        bgp_handle_ka_timer(sentinel->session);
                        break;
                    case BGP_TIMER_TYPE_HOLD:
                        bgp_handle_hold_timer(sentinel->session);
                        break;
                    case BGP_TIMER_TYPE_WORK:
                    {
                        bgp_work_sentinel_t *ws = (bgp_work_sentinel_t *)sentinel;
                        bgp_work_process(ws->inst);
                        break;
                    }
                    case BGP_TIMER_TYPE_BMP_RECONNECT:
                    {
                        bgp_bmp_instance_t *bmp =
                            (bgp_bmp_instance_t *)((char *)sentinel - offsetof(bgp_bmp_instance_t, reconnect_sentinel));
                        bgp_bmp_handle_reconnect(bmp, g_bgp_work_local->epoll_fd);
                        break;
                    }
                    case BGP_TIMER_TYPE_BMP_STATS:
                    {
                        bgp_bmp_instance_t *bmp =
                            (bgp_bmp_instance_t *)((char *)sentinel - offsetof(bgp_bmp_instance_t, stats_sentinel));
                        bgp_bmp_handle_stats_timer(bmp);
                        break;
                    }
                    case BGP_TIMER_TYPE_BMP_CONN:
                    {
                        bgp_bmp_instance_t *bmp =
                            (bgp_bmp_instance_t *)((char *)sentinel - offsetof(bgp_bmp_instance_t, conn_sentinel));
                        if (events[i].events & EPOLLOUT)
                        {
                            bgp_bmp_handle_connect_result(bmp, g_bgp_work_local->epoll_fd);
                        }
                        else
                        {
                            bgp_bmp_handle_read(bmp, g_bgp_work_local->epoll_fd);
                        }
                        break;
                    }
                    default:
                        break;
                }
                continue;
            }

            bgp_conn_t *conn = (bgp_conn_t *)events[i].data.ptr;
            if (!conn || conn->fd < 0)
            {
                continue;
            }

            if (conn->is_connecting)
            {
                bgp_handle_active_connect(conn);
            }
            else
            {
                bgp_handle_data(conn);
            }
        }
    }

    bgp_worker_runtime_cleanup();
    return NULL;
}

// ============================================================================
// session 连接管理 API
// ============================================================================

void bgp_server_start_active_conn(bgp_session_t *session)
{
    if (!session || g_bgp_work_local->epoll_fd < 0)
    {
        return;
    }
    bgp_fsm_event(session, NULL, BGP_EVT_AUTO_START, g_bgp_work_local->epoll_fd);
}

void bgp_server_stop_session_conns(bgp_session_t *session)
{
    if (!session)
    {
        return;
    }
    bgp_session_cancel_retry(session, g_bgp_work_local->epoll_fd);
    bgp_session_cancel_keepalive(session, g_bgp_work_local->epoll_fd);
    bgp_session_cancel_hold(session, g_bgp_work_local->epoll_fd);
    bgp_conn_close(session, &session->pri_conn);
    bgp_conn_close(session, &session->sec_conn);
    bgp_worker_flush_peer_routes(session->vrf ? session->vrf->vrf_id : BGP_VRF_PUBLIC_ID, &session->neighbor_addr);
    (void)bgp_vrf_purge_session_routes(session->vrf, &session->neighbor_addr);
    bgp_session_reset_negotiated(session);
    session->fsm_state = BGP_FSM_STATE_IDLE;
}

static void bgp_worker_runtime_cleanup(void)
{
    if (g_bgp_work_local->protocol && g_bgp_work_local->protocol->vrf_hash)
    {
        GHashTableIter vrf_iter;
        gpointer vrf_key, vrf_val;
        g_hash_table_iter_init(&vrf_iter, g_bgp_work_local->protocol->vrf_hash);
        while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
        {
            bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
            GHashTableIter sess_iter;
            gpointer sess_key, sess_val;
            g_hash_table_iter_init(&sess_iter, vrf->sess_hash);
            while (g_hash_table_iter_next(&sess_iter, &sess_key, &sess_val))
            {
                bgp_session_t *sess = (bgp_session_t *)sess_val;
                bgp_server_stop_session_conns(sess);
            }
        }
    }

    bgp_bmp_cleanup_all(g_bgp_work_local->epoll_fd);
    bgp_listen_stop();
    bgp_worker_cmd_drain_queue();
    bgp_work_show_cleanup();

    if (g_bgp_work_local->epoll_fd >= 0)
    {
        close(g_bgp_work_local->epoll_fd);
        g_bgp_work_local->epoll_fd = DEV_INVALID_FD;
        bgp_work_set_epoll_fd(DEV_INVALID_FD);
    }

    if (g_bgp_work_local->protocol)
    {
        bgp_protocol_destroy(g_bgp_work_local->protocol);
        g_bgp_work_local->protocol = NULL;
    }

    bgp_relay_cleanup();
}

static int bgp_worker_channel_init(void)
{
    if (!g_bgp_work_local->cmd_queue)
    {
        g_bgp_work_local->cmd_queue = g_async_queue_new();
        if (!g_bgp_work_local->cmd_queue)
        {
            LOG_ERROR("BGP: Failed to create command queue");
            return ERRCODE_FAIL;
        }
    }

    if (g_bgp_work_local->cmd_eventfd < 0)
    {
        g_bgp_work_local->cmd_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (g_bgp_work_local->cmd_eventfd < 0)
        {
            LOG_PERROR("BGP: Failed to create cmd eventfd");
            return ERRCODE_FAIL;
        }
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &bgp_cmd_tag;
    if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, g_bgp_work_local->cmd_eventfd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD cmd eventfd failed");
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

static void bgp_worker_channel_cleanup(void)
{
    if (g_bgp_work_local->epoll_fd >= 0 && g_bgp_work_local->cmd_eventfd >= 0)
    {
        epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_work_local->cmd_eventfd, NULL);
    }

    if (g_bgp_work_local->cmd_eventfd >= 0)
    {
        close(g_bgp_work_local->cmd_eventfd);
        g_bgp_work_local->cmd_eventfd = -1;
    }

    bgp_worker_cmd_drain_queue();

    if (g_bgp_work_local->cmd_queue)
    {
        g_async_queue_unref(g_bgp_work_local->cmd_queue);
        g_bgp_work_local->cmd_queue = NULL;
    }
}

int bgp_worker_prepare(void)
{
    if (!g_bgp_work_local)
    {
        g_bgp_work_local = g_malloc0(sizeof(*g_bgp_work_local));
        if (!g_bgp_work_local)
        {
            LOG_ERROR("BGP: Failed to allocate worker local context");
            return ERRCODE_FAIL;
        }
        g_bgp_work_local->epoll_fd = DEV_INVALID_FD;
        g_bgp_work_local->listen_fd = -1;
        g_bgp_work_local->cmd_eventfd = -1;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("BGP: Failed to create epoll");
        return ERRCODE_FAIL;
    }

    g_bgp_work_local->epoll_fd = epoll_fd;
    bgp_work_set_epoll_fd(epoll_fd);

    if (bgp_worker_channel_init() != ERRCODE_SUCCESS)
    {
        close(epoll_fd);
        g_bgp_work_local->epoll_fd = DEV_INVALID_FD;
        bgp_work_set_epoll_fd(DEV_INVALID_FD);
        bgp_worker_channel_cleanup();
        return ERRCODE_FAIL;
    }

    bgp_relay_init();

    return ERRCODE_SUCCESS;
}

int bgp_worker_launch(void)
{
    g_bgp_work_local->running = 1;
    if (pthread_create(&g_bgp_work_local->worker_thread, NULL, bgp_worker_thread, NULL) != 0)
    {
        LOG_PERROR("BGP: Failed to create server thread");
        g_bgp_work_local->running = 0;
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

/* bgp_worker_start_restored_sessions 已移除：
 * bgp_cfg_apply_protocol（restore 时在 server 线程执行）会调用 bgp_listen_start，
 * bgp_cfg_apply_af_neighbor（restore 时在 server 线程执行）会调用 bgp_server_start_active_conn，
 * 因此恢复后的连接启动由 server 线程在 bgp_db_restore 分发的 apply 命令中完成，无需额外函数。 */

void bgp_worker_shutdown(void)
{
    static pthread_mutex_t s_shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&s_shutdown_mutex);

    if (!g_bgp_work_local)
    {
        pthread_mutex_unlock(&s_shutdown_mutex);
        return;
    }

    bgp_worker_cmd_t *shutdown_cmd = NULL;

    if (g_bgp_work_local->worker_thread)
    {
        shutdown_cmd = bgp_worker_cmd_create(BGP_WORKER_CMD_TYPE_SHUTDOWN, NULL, TRUE);
        if (shutdown_cmd && bgp_worker_cmd_enqueue(shutdown_cmd) == 0)
        {
            (void)bgp_worker_cmd_wait(shutdown_cmd);
        }
        else
        {
            g_bgp_work_local->running = 0;
        }

        pthread_join(g_bgp_work_local->worker_thread, NULL);
        g_bgp_work_local->worker_thread = 0;
    }
    else
    {
        bgp_worker_runtime_cleanup();
    }

    if (shutdown_cmd)
    {
        bgp_worker_cmd_destroy(shutdown_cmd);
    }

    g_bgp_work_local->running = 0;
    bgp_worker_channel_cleanup();

    g_free(g_bgp_work_local);
    g_bgp_work_local = NULL;

    pthread_mutex_unlock(&s_shutdown_mutex);
}
