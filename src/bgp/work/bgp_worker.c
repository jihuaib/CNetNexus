/**
 * @file   bgp_worker.c
 * @brief  BGP worker 线程：epoll 事件循环、连接管理与命令队列
 */
#include "bgp_worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bgp_bmp_thread.h"
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

/** epoll data.ptr sentinel：区分 IPv4 listen fd 事件与连接 fd 事件 */
static char bgp_listen_tag_v4;
/** epoll data.ptr sentinel：区分 IPv6 listen fd 事件与连接 fd 事件 */
static char bgp_listen_tag_v6;
/** epoll data.ptr sentinel：区分 worker->server 命令事件 */
static char bgp_cmd_tag;
/** epoll data.ptr sentinel：区分工作事件 */
static char bgp_work_tag;

bgp_work_local_t *g_bgp_work_local = NULL;

static void bgp_worker_runtime_cleanup(void);

static gboolean bgp_worker_as_path_contains_as(const char *as_path, uint32_t asn)
{
    if (!as_path || as_path[0] == '\0' || asn == 0u)
    {
        return FALSE;
    }

    const char *p = as_path;
    while (*p != '\0')
    {
        while (*p == ' ' || *p == '\t' || *p == '{' || *p == '}' || *p == ',')
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }

        char *end = NULL;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p)
        {
            p++;
            continue;
        }
        if ((uint32_t)v == asn)
        {
            return TRUE;
        }
        p = end;
    }

    return FALSE;
}

typedef enum bgp_worker_event_type
{
    BGP_WORKER_EVENT_CALC = 1,
    BGP_WORKER_EVENT_ROUTE_FLUSH = 2,
    BGP_WORKER_EVENT_SESSION_PUB = 3,
} bgp_worker_event_type_t;

typedef struct bgp_worker_event
{
    bgp_worker_event_type_t type;
    uint32_t vrf_id;
    bgp_afi_t afi;
    bgp_safi_t safi;
} bgp_worker_event_t;

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

static void bgp_worker_signal_work_event(void)
{
    if (!g_bgp_work_local || g_bgp_work_local->work_eventfd < 0)
    {
        return;
    }

    uint64_t one = 1;
    if (write(g_bgp_work_local->work_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("BGP: work eventfd write failed");
        }
    }
}

static bgp_worker_event_t *bgp_worker_event_create(bgp_worker_event_type_t type, uint32_t vrf_id, bgp_afi_t afi,
                                                   bgp_safi_t safi)
{
    bgp_worker_event_t *evt = g_malloc(sizeof(*evt));
    if (!evt)
    {
        return NULL;
    }

    evt->type = type;
    evt->vrf_id = vrf_id;
    evt->afi = afi;
    evt->safi = safi;
    return evt;
}

static void bgp_worker_event_destroy(bgp_worker_event_t *evt)
{
    if (!evt)
    {
        return;
    }
    g_free(evt);
}

static int bgp_worker_event_enqueue(bgp_worker_event_t *evt)
{
    if (!evt || !g_bgp_work_local || !g_bgp_work_local->work_queue || g_bgp_work_local->work_eventfd < 0)
    {
        return -1;
    }

    g_async_queue_push(g_bgp_work_local->work_queue, evt);
    bgp_worker_signal_work_event();
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

static void bgp_worker_work_drain_queue(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->work_queue)
    {
        return;
    }

    bgp_worker_event_t *evt = NULL;
    while ((evt = (bgp_worker_event_t *)g_async_queue_try_pop(g_bgp_work_local->work_queue)) != NULL)
    {
        bgp_worker_event_destroy(evt);
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
        default:
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Unknown apply group_id %u.", apply->group_id);
            break;
    }
}

void bgp_worker_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd,
                                   bgp_peer_update_ingest_stats_t *stats)
{
    if (stats)
    {
        memset(stats, 0, sizeof(*stats));
    }
    if (!session || !upd)
    {
        return;
    }

    uint32_t local_as = 0u;
    if (g_bgp_work_local && g_bgp_work_local->protocol)
    {
        local_as = g_bgp_work_local->protocol->as_number;
    }

    /* 收到带本地 AS 的 reach 路由时丢弃（AS loop），但保留同报文内 withdraw 处理。 */
    if (local_as != 0u && upd->reach_len > 0 && bgp_worker_as_path_contains_as(upd->attr.as_path, local_as))
    {
        char peer[64];
        net_addr_to_str(&session->neighbor_addr, peer, sizeof(peer));
        LOG_WARN("BGP: drop UPDATE reach from %s: AS_PATH contains local AS %u (path=%s)", peer, local_as,
                 (upd->attr.as_path[0] != '\0') ? upd->attr.as_path : "-");

        bgp_update_result_t filtered = *upd;
        filtered.reach = NULL;
        filtered.reach_len = 0;

        bgp_peer_update_ingest_stats_t relay_stats = {0};
        bgp_relay_ingest_peer_update(session, &filtered, &relay_stats);
        relay_stats.reach_failed += upd->reach_len;

        if (stats)
        {
            *stats = relay_stats;
        }
        return;
    }

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

int bgp_worker_post_calc_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_worker_event_t *evt = bgp_worker_event_create(BGP_WORKER_EVENT_CALC, vrf_id, afi, safi);
    if (!evt)
    {
        return -1;
    }
    if (bgp_worker_event_enqueue(evt) != 0)
    {
        bgp_worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

int bgp_worker_post_route_flush_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_worker_event_t *evt = bgp_worker_event_create(BGP_WORKER_EVENT_ROUTE_FLUSH, vrf_id, afi, safi);
    if (!evt)
    {
        return -1;
    }
    if (bgp_worker_event_enqueue(evt) != 0)
    {
        bgp_worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

int bgp_worker_post_session_pub_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_worker_event_t *evt = bgp_worker_event_create(BGP_WORKER_EVENT_SESSION_PUB, vrf_id, afi, safi);
    if (!evt)
    {
        return -1;
    }
    if (bgp_worker_event_enqueue(evt) != 0)
    {
        bgp_worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

void bgp_worker_drain_work_events(void)
{
    if (!g_bgp_work_local || g_bgp_work_local->work_eventfd < 0 || !g_bgp_work_local->work_queue)
    {
        return;
    }

    uint64_t v;
    while (read(g_bgp_work_local->work_eventfd, &v, sizeof(v)) > 0)
    {
        /* drain eventfd counter */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("BGP: work eventfd read failed");
    }

    int processed = 0;
    bgp_worker_event_t *evt = NULL;
    while (processed < BGP_WORK_BATCH_SIZE &&
           (evt = (bgp_worker_event_t *)g_async_queue_try_pop(g_bgp_work_local->work_queue)) != NULL)
    {
        switch (evt->type)
        {
            case BGP_WORKER_EVENT_CALC:
                bgp_work_handle_calc_event(evt->vrf_id, evt->afi, evt->safi);
                break;
            case BGP_WORKER_EVENT_ROUTE_FLUSH:
                bgp_work_handle_route_flush_event(evt->vrf_id, evt->afi, evt->safi);
                break;
            case BGP_WORKER_EVENT_SESSION_PUB:
                bgp_work_handle_session_pub_event(evt->vrf_id, evt->afi, evt->safi);
                break;
            default:
                LOG_WARN("BGP: unknown work event type=%d", (int)evt->type);
                break;
        }

        bgp_worker_event_destroy(evt);
        processed++;
    }

    if (g_async_queue_length(g_bgp_work_local->work_queue) > 0)
    {
        bgp_worker_signal_work_event();
    }
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
    if (g_bgp_work_local->listen_fd >= 0 || g_bgp_work_local->listen_fd_v6 >= 0)
    {
        return; /* 已在监听，幂等 */
    }

    int fd4 = socket(AF_INET, SOCK_STREAM, 0);
    if (fd4 >= 0)
    {
        int opt = 1;
        (void)setsockopt(fd4, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        (void)setsockopt(fd4, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_addr.s_addr = INADDR_ANY;
        addr4.sin_port = htons(BGP_LISTEN_PORT);

        if (bind(fd4, (struct sockaddr *)&addr4, sizeof(addr4)) < 0)
        {
            LOG_PERROR("BGP: bind 0.0.0.0:179 failed");
            close(fd4);
            fd4 = -1;
        }
        else if (listen(fd4, 32) < 0)
        {
            LOG_PERROR("BGP: listen IPv4 failed");
            close(fd4);
            fd4 = -1;
        }
        else
        {
            struct epoll_event ev4;
            memset(&ev4, 0, sizeof(ev4));
            ev4.events = EPOLLIN;
            ev4.data.ptr = &bgp_listen_tag_v4;
            if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, fd4, &ev4) < 0)
            {
                LOG_PERROR("BGP: epoll_ctl ADD IPv4 listen fd failed");
                close(fd4);
                fd4 = -1;
            }
        }
    }

    int fd6 = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd6 >= 0)
    {
        int opt = 1;
        int v6_only = 1;
        (void)setsockopt(fd6, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        (void)setsockopt(fd6, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        (void)setsockopt(fd6, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));

        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr = in6addr_any;
        addr6.sin6_port = htons(BGP_LISTEN_PORT);

        if (bind(fd6, (struct sockaddr *)&addr6, sizeof(addr6)) < 0)
        {
            LOG_PERROR("BGP: bind [::]:179 failed");
            close(fd6);
            fd6 = -1;
        }
        else if (listen(fd6, 32) < 0)
        {
            LOG_PERROR("BGP: listen IPv6 failed");
            close(fd6);
            fd6 = -1;
        }
        else
        {
            struct epoll_event ev6;
            memset(&ev6, 0, sizeof(ev6));
            ev6.events = EPOLLIN;
            ev6.data.ptr = &bgp_listen_tag_v6;
            if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, fd6, &ev6) < 0)
            {
                LOG_PERROR("BGP: epoll_ctl ADD IPv6 listen fd failed");
                close(fd6);
                fd6 = -1;
            }
        }
    }

    g_bgp_work_local->listen_fd = fd4;
    g_bgp_work_local->listen_fd_v6 = fd6;

    if (fd4 >= 0)
    {
        LOG_INFO("BGP: Listening on 0.0.0.0:179 (fd=%d)", fd4);
    }
    if (fd6 >= 0)
    {
        LOG_INFO("BGP: Listening on [::]:179 (fd=%d)", fd6);
    }
    if (fd4 < 0 && fd6 < 0)
    {
        LOG_ERROR("BGP: Failed to start listen sockets on both IPv4 and IPv6");
    }
}

void bgp_listen_stop(void)
{
    if (g_bgp_work_local->listen_fd < 0 && g_bgp_work_local->listen_fd_v6 < 0)
    {
        return;
    }

    if (g_bgp_work_local->listen_fd >= 0)
    {
        if (g_bgp_work_local->epoll_fd >= 0)
        {
            epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_work_local->listen_fd, NULL);
        }
        close(g_bgp_work_local->listen_fd);
        g_bgp_work_local->listen_fd = -1;
    }

    if (g_bgp_work_local->listen_fd_v6 >= 0)
    {
        if (g_bgp_work_local->epoll_fd >= 0)
        {
            epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_work_local->listen_fd_v6, NULL);
        }
        close(g_bgp_work_local->listen_fd_v6);
        g_bgp_work_local->listen_fd_v6 = -1;
    }

    LOG_INFO("BGP: Stopped listening on 0.0.0.0:179 and [::]:179");
}

// ============================================================================
// BGP server 辅助函数
// ============================================================================

/**
 * @brief 从 epoll 移除、销毁连接对象，并将 session 槽位置 NULL
 * @param slot &sess->pri_conn 或 &sess->sec_conn
 */

// ============================================================================
// BGP server 线程 — 事件处理函数
// ============================================================================

/**
 * @brief 处理指定 listener 上的被动入站连接
 */
static void bgp_handle_passive_accept(int listen_fd)
{
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    struct sockaddr_storage peer_sa;
    socklen_t addr_len = sizeof(peer_sa);
    int conn_fd = accept(listen_fd, (struct sockaddr *)&peer_sa, &addr_len);

    if (conn_fd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("BGP: accept failed");
        }
        return;
    }

    /* 被动连接必须设为非阻塞，否则 recv() 会阻塞 worker 线程 */
    int flags = fcntl(conn_fd, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK);
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

    if (sess->pri_conn && !sess->pri_conn->is_connecting && sess->fsm_state == BGP_FSM_STATE_ESTABLISHED)
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
        /* 碰撞场景：pri_conn 已存在，新连接暂挂为 sec_conn，直接发 OPEN（不经 FSM） */
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

        /* 向 sec_conn 发送 OPEN，碰撞将在收到对端 OPEN 时解决 */
        bgp_protocol_t *proto = g_bgp_work_local->protocol;
        bgp_vrf_t *vrf0 = proto ? bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID) : NULL;
        GList *af_peers = vrf0 ? bgp_vrf_get_session_peers(vrf0, &sess->neighbor_addr) : NULL;
        bgp_pkt_send_open(conn, proto ? proto->as_number : 0, vrf0 ? vrf0->router_id : 0, af_peers);
        g_list_free(af_peers);
        /* FSM 状态不变（跟踪 pri_conn） */
    }
    else
    {
        sess->pri_conn = conn;
        sess->pri_last_socket_error = 0;
        /* 触发 FSM 事件：发送 OPEN 并迁移状态 */
        bgp_fsm_event(sess, BGP_EVT_TCP_CONNECTION_CONFIRMED);
    }
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
        bgp_fsm_event(sess, BGP_EVT_TCP_CONNECTION_FAILS);
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

    bgp_fsm_event(sess, BGP_EVT_TCP_CR_ACKED);
}

static void bgp_handle_ka_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->ka_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read ka timerfd");
    }
    bgp_fsm_event(sess, BGP_EVT_KEEPALIVE_TIMER_EXPIRED);
}

static void bgp_handle_hold_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->hold_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read hold timerfd");
    }
    bgp_fsm_event(sess, BGP_EVT_HOLD_TIMER_EXPIRED);
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

    bgp_fsm_event(sess, BGP_EVT_CONNECT_RETRY_EXPIRED);
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

            if (events[i].data.ptr == (void *)&bgp_listen_tag_v4)
            {
                bgp_handle_passive_accept(g_bgp_work_local->listen_fd);
                continue;
            }
            if (events[i].data.ptr == (void *)&bgp_listen_tag_v6)
            {
                bgp_handle_passive_accept(g_bgp_work_local->listen_fd_v6);
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
            if (events[i].data.ptr == (void *)&bgp_work_tag)
            {
                bgp_worker_drain_work_events();
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
                bgp_pkt_on_data(conn);
            }
        }

        /* 释放本轮 epoll 事件处理期间关闭的连接 */
        bgp_worker_flush_deferred_conns();
    }

    bgp_worker_flush_deferred_conns();
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
    bgp_fsm_event(session, BGP_EVT_AUTO_START);
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
    bgp_conn_close(session, &session->pri_conn, g_bgp_work_local->epoll_fd);
    bgp_conn_close(session, &session->sec_conn, g_bgp_work_local->epoll_fd);
    bgp_worker_flush_peer_routes(session->vrf ? session->vrf->vrf_id : BGP_VRF_PUBLIC_ID, &session->neighbor_addr);
    (void)bgp_vrf_purge_session_routes(session->vrf, &session->neighbor_addr);
    bgp_session_reset_negotiated(session);
    session->fsm_state = BGP_FSM_STATE_IDLE;
}

void bgp_worker_flush_deferred_conns(void)
{
    GSList *list = g_bgp_work_local->deferred_conns;
    g_bgp_work_local->deferred_conns = NULL;
    for (GSList *n = list; n; n = n->next)
    {
        g_free(n->data);
    }
    g_slist_free(list);
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

    bgp_listen_stop();
    bgp_worker_cmd_drain_queue();
    bgp_work_show_cleanup();

    if (g_bgp_work_local->epoll_fd >= 0)
    {
        close(g_bgp_work_local->epoll_fd);
        g_bgp_work_local->epoll_fd = DEV_INVALID_FD;
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
    if (!g_bgp_work_local->work_queue)
    {
        g_bgp_work_local->work_queue = g_async_queue_new();
        if (!g_bgp_work_local->work_queue)
        {
            LOG_ERROR("BGP: Failed to create work event queue");
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
    if (g_bgp_work_local->work_eventfd < 0)
    {
        g_bgp_work_local->work_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (g_bgp_work_local->work_eventfd < 0)
        {
            LOG_PERROR("BGP: Failed to create work eventfd");
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

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &bgp_work_tag;
    if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, g_bgp_work_local->work_eventfd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD work eventfd failed");
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
    if (g_bgp_work_local->epoll_fd >= 0 && g_bgp_work_local->work_eventfd >= 0)
    {
        epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_work_local->work_eventfd, NULL);
    }

    if (g_bgp_work_local->cmd_eventfd >= 0)
    {
        close(g_bgp_work_local->cmd_eventfd);
        g_bgp_work_local->cmd_eventfd = -1;
    }
    if (g_bgp_work_local->work_eventfd >= 0)
    {
        close(g_bgp_work_local->work_eventfd);
        g_bgp_work_local->work_eventfd = -1;
    }

    bgp_worker_cmd_drain_queue();
    bgp_worker_work_drain_queue();

    if (g_bgp_work_local->cmd_queue)
    {
        g_async_queue_unref(g_bgp_work_local->cmd_queue);
        g_bgp_work_local->cmd_queue = NULL;
    }
    if (g_bgp_work_local->work_queue)
    {
        g_async_queue_unref(g_bgp_work_local->work_queue);
        g_bgp_work_local->work_queue = NULL;
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
        g_bgp_work_local->listen_fd_v6 = -1;
        g_bgp_work_local->cmd_eventfd = -1;
        g_bgp_work_local->work_eventfd = -1;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("BGP: Failed to create epoll");
        return ERRCODE_FAIL;
    }

    g_bgp_work_local->epoll_fd = epoll_fd;

    if (bgp_worker_channel_init() != ERRCODE_SUCCESS)
    {
        close(epoll_fd);
        g_bgp_work_local->epoll_fd = DEV_INVALID_FD;
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

void bgp_worker_handle_bmp_initial_peers(const char *inst_name)
{
    if (!inst_name || !g_bgp_work_local || !g_bgp_work_local->protocol)
    {
        return;
    }

    GHashTable *vrf_hash = g_bgp_work_local->protocol->vrf_hash;
    if (!vrf_hash)
    {
        return;
    }

    GHashTableIter vrf_iter;
    gpointer vrf_key, vrf_val;
    g_hash_table_iter_init(&vrf_iter, vrf_hash);

    while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
        if (!vrf || !vrf->sess_hash)
        {
            continue;
        }

        GHashTableIter sess_iter;
        gpointer sess_key, sess_val;
        g_hash_table_iter_init(&sess_iter, vrf->sess_hash);

        while (g_hash_table_iter_next(&sess_iter, &sess_key, &sess_val))
        {
            bgp_session_t *sess = (bgp_session_t *)sess_val;
            if (!sess || sess->fsm_state != BGP_FSM_STATE_ESTABLISHED)
            {
                continue;
            }

            bgp_bmp_peer_info_t info;
            bgp_bmp_fill_peer_info(sess, &info);
            bgp_bmp_post_initial_peer_up(&info, inst_name);
        }
    }
}

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
