/**
 * @file   bgp_worker.c
 * @brief  BGP worker 线程：epoll 事件循环、连接管理与命令队列
 */
#include "bgp_worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bgp_calc.h"
#include "bgp_cfg_apply.h"
#include "bgp_cli.h"
#include "bgp_conn.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
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

static void bgp_worker_runtime_cleanup(void);

// ============================================================================
// 命令队列（worker -> server）
// ============================================================================

typedef enum bgp_worker_cmd_type
{
    BGP_WORKER_CMD_TYPE_SHOW_CLI = 1, /**< show CLI 命令派发（CLI_MSG_TYPE/CLI_MSG_TYPE_CONTINUE） */
    BGP_WORKER_CMD_TYPE_SHUTDOWN = 2,
    BGP_WORKER_CMD_TYPE_APPLY = 3,     /**< 跨线程配置应用命令 */
    BGP_WORKER_CMD_TYPE_ROUTE_MSG = 4, /**< ROUTE_MSG_TYPE_UPDATE/REPORT */
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
    if (!cmd || !g_bgp_local || !g_bgp_local->cmd_queue || g_bgp_local->cmd_eventfd < 0)
    {
        return -1;
    }

    g_async_queue_push(g_bgp_local->cmd_queue, cmd);

    uint64_t one = 1;
    if (write(g_bgp_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
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
    if (!g_bgp_local || !g_bgp_local->cmd_queue)
    {
        return;
    }

    bgp_worker_cmd_t *cmd = NULL;
    while ((cmd = (bgp_worker_cmd_t *)g_async_queue_try_pop(g_bgp_local->cmd_queue)) != NULL)
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
 * @brief 重置 VRF 内所有有活跃连接的 session（router-id / timer 变更时使用）
 */
static void server_vrf_reset_all_sessions(bgp_vrf_t *vrf)
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
            bgp_neighbor_down(sess, g_bgp_local->epoll_fd);
        }
    }
}

/**
 * @brief 按当前 VRF connect-retry 配置，重排已挂起的 retry 定时器
 *
 * 用于处理 connect-retry 参数动态修改：已存在的 retry timer 需要立即生效新间隔。
 */
static void server_vrf_rearm_retry_timers(bgp_vrf_t *vrf)
{
    if (!vrf || !vrf->sess_hash || !g_bgp_local)
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
        bgp_session_cancel_retry(sess, g_bgp_local->epoll_fd);
        bgp_session_arm_retry(sess, g_bgp_local->epoll_fd, retry_sec);
    }
}

/**
 * @brief 在 server 线程中执行结构化配置应用命令
 *
 * 负责参数校验、同配置短路和调用 bgp_cfg_apply_*。将结果写入 apply->rc 和相关输出字段。
 */
static void bgp_worker_dispatch_apply_cmd(bgp_apply_cmd_t *apply)
{
    if (!apply)
    {
        return;
    }
    apply->rc = BGP_APPLY_RC_FAIL;
    apply->errmsg[0] = '\0';

    const gboolean is_no = apply->isNo ? TRUE : FALSE;

    bgp_vrf_t *vrf = NULL;
    bgp_protocol_t *proto = g_bgp_local ? g_bgp_local->protocol : NULL;

    /* 需要 VRF 的命令类型，提前查找 */
    if (apply->group_id != BGP_CLI_GROUP_ID_PROTOCOL && proto)
    {
        vrf = bgp_protocol_get_vrf(proto, apply->vrf_id);
    }

    switch (apply->group_id)
    {
        case BGP_CLI_GROUP_ID_PROTOCOL:
        {
            if (is_no)
            {
                if (!proto)
                {
                    apply->rc = BGP_APPLY_RC_NOOP;
                    return;
                }
            }
            else
            {
                if (proto)
                {
                    if (proto->as_number == apply->u.protocol.as_number)
                    {
                        apply->rc = BGP_APPLY_RC_NOOP;
                        return;
                    }
                    snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: AS number mismatch.");
                    return;
                }
                if (apply->u.protocol.as_number == 0)
                {
                    snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Missing AS number.");
                    return;
                }
            }
            uint32_t ret = bgp_cfg_apply_protocol(is_no, apply->u.protocol.as_number);
            if (ret != ERRCODE_SUCCESS)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply protocol configuration.");
                return;
            }
            apply->rc = BGP_APPLY_RC_OK;
            break;
        }

        case BGP_CLI_GROUP_ID_NEIGHBOR:
        {
            if (!proto)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg),
                         "BGP Error: BGP not configured. Run 'bgp <as-number>' first.");
                return;
            }
            if (!vrf)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
                return;
            }
            bgp_session_t *existing = bgp_vrf_find_session(vrf, &apply->u.neighbor.addr);
            if (is_no)
            {
                if (!existing)
                {
                    apply->rc = BGP_APPLY_RC_NOOP;
                    return;
                }
            }
            else
            {
                if (existing && existing->remote_as == apply->u.neighbor.remote_as)
                {
                    apply->rc = BGP_APPLY_RC_NOOP;
                    return;
                }
            }
            uint32_t ret = bgp_cfg_apply_neighbor(is_no, vrf, &apply->u.neighbor.addr, apply->u.neighbor.remote_as);
            if (ret != ERRCODE_SUCCESS)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply neighbor configuration.");
                return;
            }
            apply->rc = BGP_APPLY_RC_OK;
            break;
        }

        case BGP_CLI_GROUP_ID_ADDR_FAMILY:
        {
            if (!proto)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
                return;
            }
            if (!vrf)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
                return;
            }
            bgp_instance_t *inst =
                g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(apply->u.instance.afi, apply->u.instance.safi));
            if (is_no && !inst)
            {
                apply->rc = BGP_APPLY_RC_NOOP;
                return;
            }
            if (!is_no && inst)
            {
                apply->rc = BGP_APPLY_RC_NOOP;
                return;
            }
            uint32_t ret = bgp_cfg_apply_instance(is_no, vrf, apply->u.instance.afi, apply->u.instance.safi);
            if (ret != ERRCODE_SUCCESS)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply instance configuration.");
                return;
            }
            apply->rc = BGP_APPLY_RC_OK;
            break;
        }

        case BGP_CLI_GROUP_ID_AF_NEIGHBOR:
        {
            if (!proto)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
                return;
            }
            if (!vrf)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
                return;
            }
            bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.af_neighbor.addr);
            gpointer inst_key = bgp_inst_hash_key(apply->u.af_neighbor.afi, apply->u.af_neighbor.safi);
            bgp_instance_t *inst_before = vrf ? g_hash_table_lookup(vrf->inst_hash, inst_key) : NULL;
            gboolean had_af_before = (inst_before && inst_before->peer_hash &&
                                      g_hash_table_lookup(inst_before->peer_hash, &apply->u.af_neighbor.addr));

            /* 仅当“变更前”已有连接时，才考虑变更后重协商。 */
            gboolean had_conn_before = (sess && (sess->pri_conn || sess->sec_conn));
            if (!is_no && !sess)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg),
                         "BGP Error: Neighbor session not configured. Run 'neighbor <ip> as <as>' first.");
                return;
            }
            uint32_t ret = bgp_cfg_apply_af_neighbor(is_no, vrf, apply->u.af_neighbor.afi, apply->u.af_neighbor.safi,
                                                     &apply->u.af_neighbor.addr);
            if (ret != ERRCODE_SUCCESS)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply AF neighbor configuration.");
                return;
            }
            bgp_instance_t *inst_after = vrf ? g_hash_table_lookup(vrf->inst_hash, inst_key) : NULL;
            gboolean has_af_after = (inst_after && inst_after->peer_hash &&
                                     g_hash_table_lookup(inst_after->peer_hash, &apply->u.af_neighbor.addr));
            gboolean af_changed = (had_af_before != has_af_after);

            /* 仅在 AF 成员关系实际变化时触发重协商，避免重复 enable 导致无意义 reset。 */
            if (had_conn_before && af_changed && sess && (sess->pri_conn || sess->sec_conn))
            {
                bgp_neighbor_down(sess, g_bgp_local->epoll_fd);
            }
            apply->rc = BGP_APPLY_RC_OK;
            break;
        }

        case BGP_CLI_GROUP_ID_ROUTER_ID:
        {
            if (!proto)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
                return;
            }
            if (!vrf)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
                return;
            }
            /* 同配置短路 */
            if (is_no && vrf->router_id == 0)
            {
                apply->rc = BGP_APPLY_RC_NOOP;
                return;
            }
            if (!is_no && apply->u.router_id.id[0] != '\0')
            {
                struct in_addr cmp;
                if (inet_pton(AF_INET, apply->u.router_id.id, &cmp) == 1 && ntohl(cmp.s_addr) == vrf->router_id)
                {
                    apply->rc = BGP_APPLY_RC_NOOP;
                    return;
                }
            }
            uint32_t ret = bgp_cfg_apply_router_id(is_no, vrf, apply->u.router_id.id);
            if (ret != ERRCODE_SUCCESS)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply router-id configuration.");
                return;
            }
            server_vrf_reset_all_sessions(vrf);
            apply->rc = BGP_APPLY_RC_OK;
            break;
        }

        case BGP_CLI_GROUP_ID_TIMERS:
        {
            if (!proto)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
                return;
            }
            if (!vrf)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
                return;
            }
            /* 同配置短路 */
            if (is_no && vrf->keepalive == BGP_TIMER_DEFAULT_KEEPALIVE && vrf->hold_time == BGP_TIMER_DEFAULT_HOLD)
            {
                apply->rc = BGP_APPLY_RC_NOOP;
                return;
            }
            if (!is_no && apply->u.timers.keepalive == vrf->keepalive && apply->u.timers.hold_time == vrf->hold_time)
            {
                apply->rc = BGP_APPLY_RC_NOOP;
                return;
            }
            uint32_t ret = bgp_cfg_apply_timers(is_no, vrf, apply->u.timers.keepalive, apply->u.timers.hold_time);
            if (ret != ERRCODE_SUCCESS)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply timer configuration.");
                return;
            }
            server_vrf_reset_all_sessions(vrf);
            apply->rc = BGP_APPLY_RC_OK;
            break;
        }

        case BGP_CLI_GROUP_ID_CONNECT_RETRY:
        {
            if (!proto)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
                return;
            }
            if (!vrf)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
                return;
            }
            /* 同配置短路 */
            if (is_no && vrf->connect_retry == BGP_TIMER_DEFAULT_CONNECT_RETRY)
            {
                apply->rc = BGP_APPLY_RC_NOOP;
                return;
            }
            if (!is_no && apply->u.connect_retry.interval == vrf->connect_retry)
            {
                apply->rc = BGP_APPLY_RC_NOOP;
                return;
            }
            uint32_t ret = bgp_cfg_apply_connect_retry(is_no, vrf, apply->u.connect_retry.interval);
            if (ret != ERRCODE_SUCCESS)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg),
                         "BGP Error: Failed to apply connect-retry configuration.");
                return;
            }
            server_vrf_rearm_retry_timers(vrf);
            apply->rc = BGP_APPLY_RC_OK;
            break;
        }

        case BGP_CLI_GROUP_ID_OPEN_CAP:
        {
            if (!proto)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
                return;
            }
            if (!vrf)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
                return;
            }
            bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.open_cap.addr);
            if (!sess)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Neighbor not found.");
                return;
            }
            uint32_t ret = bgp_cfg_apply_open_capability(is_no, sess, apply->u.open_cap.cap_bit);
            if (ret != ERRCODE_SUCCESS)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Failed to apply capability configuration.");
                return;
            }
            apply->out.sess_flags = sess->flags;
            bgp_neighbor_down(sess, g_bgp_local->epoll_fd);
            apply->rc = BGP_APPLY_RC_OK;
            break;
        }

        case BGP_CLI_GROUP_ID_IMPORT_ROUTE:
        {
            if (!proto)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
                return;
            }
            if (!vrf)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
                return;
            }
            bgp_instance_t *inst =
                bgp_vrf_get_or_create_instance(vrf, apply->u.import_route.afi, apply->u.import_route.safi);
            if (!inst)
            {
                snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Address family instance creation failed.");
                return;
            }
            uint32_t proto_mask = 1u << apply->u.import_route.import_proto;
            if (is_no)
            {
                inst->import_protos &= ~proto_mask;
            }
            else
            {
                inst->import_protos |= proto_mask;
            }
            apply->out.import_protos = inst->import_protos;
            apply->rc = BGP_APPLY_RC_OK;
            break;
        }

        default:
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Unknown apply group_id %u.", apply->group_id);
            break;
    }
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

    if (!g_bgp_local || !g_bgp_local->protocol)
    {
        return 0;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, entry->vrf_id);
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

    if (!inst || !(inst->import_protos & (1u << entry->protocol)))
    {
        /* 该 AF 未配置 import-route 对应协议，丢弃 */
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
            bgp_calc_queue_push(inst->calc_queue, &nlri);
        }
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(&nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: Import route withdraw %s", nlri_str);
    }
    else
    {
        /* 构建合成 BGP 属性（ORIGIN=INCOMPLETE，AS_PATH 为空） */
        bgp_attr_t attr;
        memset(&attr, 0, sizeof(attr));
        attr.origin = BGP_ORIGIN_INCOMPLETE;
        attr.local_pref = 100;
        attr.has_local_pref = true;

        bgp_nexthop_t nexthop;
        memset(&nexthop, 0, sizeof(nexthop));
        nexthop.has_link_local = false;
        nexthop.global = entry->nexthop_addr;

        int rc = bgp_rib_reach_one(inst->rib, &nlri, &src, &attr, &nexthop);
        /* 与对端 UPDATE 处理保持一致：新增/更新都触发 best-path 与发布流程 */
        if (rc >= 0 && inst->calc_queue)
        {
            bgp_calc_queue_push(inst->calc_queue, &nlri);
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
            (void)bgp_cli_handle_show_msg(msg);
            break;
        case CLI_MSG_TYPE_CONTINUE:
            (void)bgp_cli_handle_continue(msg);
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

        default:
            LOG_WARN("BGP: unsupported route message type=0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

static gboolean bgp_process_cmd_event(void)
{
    if (!g_bgp_local || g_bgp_local->cmd_eventfd < 0 || !g_bgp_local->cmd_queue)
    {
        return FALSE;
    }

    uint64_t v;
    while (read(g_bgp_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
        /* drain eventfd counter */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("BGP: cmd eventfd read failed");
    }

    gboolean stop = FALSE;
    bgp_worker_cmd_t *cmd = NULL;
    while ((cmd = (bgp_worker_cmd_t *)g_async_queue_try_pop(g_bgp_local->cmd_queue)) != NULL)
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
                g_bgp_local->running = 0;
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
    if (!g_bgp_local || g_bgp_local->epoll_fd < 0)
    {
        return;
    }
    if (g_bgp_local->listen_fd >= 0)
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
    if (epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD listen fd failed");
        close(fd);
        return;
    }

    g_bgp_local->listen_fd = fd;
    LOG_INFO("BGP: Listening on 0.0.0.0:179 (fd=%d)", fd);
}

void bgp_listen_stop(void)
{
    if (!g_bgp_local || g_bgp_local->listen_fd < 0)
    {
        return;
    }
    if (g_bgp_local->epoll_fd >= 0)
    {
        epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_local->listen_fd, NULL);
    }
    close(g_bgp_local->listen_fd);
    g_bgp_local->listen_fd = -1;
    LOG_INFO("BGP: Stopped listening on 0.0.0.0:179");
}

// ============================================================================
// BGP server 辅助函数
// ============================================================================

/**
 * @brief 从 epoll 移除、销毁连接对象，并将 session 槽位置 NULL
 * @param slot &sess->pri_conn 或 &sess->sec_conn
 */
static void bgp_conn_close(bgp_conn_t **slot)
{
    if (!slot || !*slot)
    {
        return;
    }
    bgp_conn_t *conn = *slot;
    if (conn->fd >= 0)
    {
        epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
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
}

/* 前向声明：connect-retry 调度辅助（定义在 bgp_handle_retry_timer 之前） */
static void bgp_arm_retry(bgp_session_t *sess);

// ============================================================================
// BGP server 线程 — 事件处理函数
// ============================================================================

/**
 * @brief 处理全局 listener 上的被动入站连接
 */
static void bgp_handle_passive_accept(void)
{
    bgp_protocol_t *proto = g_bgp_local->protocol;

    struct sockaddr_storage peer_sa;
    socklen_t addr_len = sizeof(peer_sa);
    int conn_fd = accept(g_bgp_local->listen_fd, (struct sockaddr *)&peer_sa, &addr_len);

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
    if (epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0)
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
    }
    else
    {
        sess->pri_conn = conn;
    }

    GList *af_peers = bgp_vrf_get_session_peers(vrf0, &from_addr);
    bgp_pkt_send_open(conn, proto->as_number, vrf0->router_id, af_peers);
    g_list_free(af_peers);
}

/**
 * @brief 处理主动连接完成事件（EPOLLOUT）
 */
static void bgp_handle_active_connect(bgp_conn_t *conn)
{
    bgp_session_t *sess = conn->session;
    bgp_protocol_t *proto = g_bgp_local->protocol;

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len);

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    if (err != 0)
    {
        LOG_WARN("BGP: Active connection to %s failed: %s (fd=%d)", addr_str, strerror(err), conn->fd);
        bgp_conn_close(&sess->pri_conn);
        if (sess->sec_conn)
        {
            bgp_session_promote_sec(sess);
        }
        else
        {
            bgp_arm_retry(sess);
        }
        return;
    }

    if (sess->sec_conn)
    {
        LOG_INFO("BGP: Active TCP to %s established (fd=%d), sec_conn fd=%d also present, §6.8 collision pending",
                 addr_str, conn->fd, sess->sec_conn->fd);
    }
    else
    {
        LOG_INFO("BGP: Active TCP connection to %s established (fd=%d)", addr_str, conn->fd);
    }
    conn->is_connecting = FALSE;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);

    if (proto)
    {
        bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
        GList *af_peers = vrf0 ? bgp_vrf_get_session_peers(vrf0, &sess->neighbor_addr) : NULL;
        bgp_pkt_send_open(conn, proto->as_number, vrf0 ? vrf0->router_id : 0, af_peers);
        g_list_free(af_peers);
    }
}

/** bgp_rib_foreach_best 回调上下文：补发时使用 */
typedef struct
{
    bgp_conn_t *conn;
    uint32_t sent;
} reannounce_ctx_t;

/** bgp_rib_foreach_best 回调：向指定连接发送单条 best-route UPDATE */
static void reannounce_best_cb(const bgp_rthead_t *head, const bgp_route_node_t *route, gpointer user_data)
{
    reannounce_ctx_t *ctx = user_data;
    bgp_pkt_send_update(ctx->conn, &head->nlri, &route->attr, &route->nexthop);
    ctx->sent++;
}

/**
 * @brief 邻居会话建连后，向该连接补发当前 AF best-route 快照
 */
static void bgp_reannounce_best_to_conn(bgp_session_t *sess, bgp_conn_t *conn)
{
    if (!sess || !conn || conn->fd < 0 || !sess->vrf)
    {
        return;
    }

    reannounce_ctx_t ctx = {.conn = conn, .sent = 0};
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, sess->vrf->inst_hash);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        (void)key;
        bgp_instance_t *inst = (bgp_instance_t *)val;
        if (!inst || !inst->rib || !inst->peer_hash)
        {
            continue;
        }

        if (!g_hash_table_lookup(inst->peer_hash, &sess->neighbor_addr))
        {
            continue;
        }

        bgp_rib_foreach_best(inst->rib, reannounce_best_cb, &ctx);
    }

    if (ctx.sent > 0)
    {
        char nbr[64];
        net_addr_to_str(&sess->neighbor_addr, nbr, sizeof(nbr));
        LOG_INFO("BGP: Re-announced %u best route(s) to %s on session establish", ctx.sent, nbr);
    }
}

/**
 * @brief 处理已建立连接上的 BGP 数据（EPOLLIN）
 */
static void bgp_handle_data(bgp_conn_t *conn)
{
    bgp_session_t *sess = conn->session;
    bgp_conn_t **slot = (sess->pri_conn == conn) ? &sess->pri_conn : &sess->sec_conn;
    gboolean was_active = conn->is_active;
    bgp_conn_state_t old_state = conn->state;

    int ret = bgp_pkt_on_data(conn);

    if (ret == BGP_PKT_ON_DATA_COLLISION_CLOSE_ME)
    {
        char addr_str[64];
        net_addr_to_str(&conn->peer_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: §6.8 collision: closing %s connection (fd=%d) with %s", conn->is_active ? "active" : "passive",
                 conn->fd, addr_str);
        bgp_conn_close(slot);
        if (!sess->pri_conn && sess->sec_conn)
        {
            bgp_session_promote_sec(sess);
        }
        return;
    }

    if (ret == BGP_PKT_ON_DATA_COLLISION_CLOSE_OTHER)
    {
        bgp_conn_t **other_slot = (slot == &sess->pri_conn) ? &sess->sec_conn : &sess->pri_conn;
        if (*other_slot)
        {
            char addr_str[64];
            net_addr_to_str(&(*other_slot)->peer_addr, addr_str, sizeof(addr_str));
            LOG_INFO("BGP: §6.8 collision: closing %s connection (fd=%d) with %s",
                     (*other_slot)->is_active ? "active" : "passive", (*other_slot)->fd, addr_str);
            bgp_conn_close(other_slot);
        }
        if (!sess->pri_conn && sess->sec_conn)
        {
            bgp_session_promote_sec(sess);
        }
        return;
    }

    if (ret < 0)
    {
        bgp_session_cancel_keepalive(sess, g_bgp_local->epoll_fd);
        bgp_session_cancel_hold(sess, g_bgp_local->epoll_fd);

        char addr_str[64];
        net_addr_to_str(&conn->peer_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: Connection with %s closed (fd=%d)", addr_str, conn->fd);
        bgp_conn_close(slot);

        if (!sess->pri_conn && !sess->sec_conn)
        {
            (void)bgp_vrf_purge_session_routes(sess->vrf, &sess->neighbor_addr);
        }

        if (was_active && !sess->pri_conn)
        {
            bgp_arm_retry(sess);
        }
        return;
    }

    if (old_state != BGP_CONN_STATE_ESTABLISHED && conn->state == BGP_CONN_STATE_ESTABLISHED)
    {
        bgp_protocol_t *proto = g_bgp_local->protocol;
        bgp_vrf_t *vrf0 = proto ? bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID) : NULL;
        uint16_t ka_sec = vrf0 ? vrf0->keepalive : BGP_TIMER_DEFAULT_KEEPALIVE;
        bgp_session_arm_keepalive(sess, g_bgp_local->epoll_fd, ka_sec);
        if (sess->negotiated_hold > 0)
        {
            bgp_session_arm_hold(sess, g_bgp_local->epoll_fd, sess->negotiated_hold);
        }

        bgp_reannounce_best_to_conn(sess, conn);
    }

    if (sess->hold_reset_pending)
    {
        sess->hold_reset_pending = FALSE;
        bgp_session_reset_hold(sess);
    }
}

// ============================================================================
// connect-retry 定时器调度辅助
// ============================================================================

static void bgp_arm_retry(bgp_session_t *sess)
{
    bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(g_bgp_local->protocol, BGP_VRF_PUBLIC_ID);
    uint16_t retry_sec = vrf0 ? vrf0->connect_retry : BGP_TIMER_DEFAULT_CONNECT_RETRY;
    bgp_session_arm_retry(sess, g_bgp_local->epoll_fd, retry_sec);
}

static void bgp_handle_ka_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->ka_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read ka timerfd");
    }

    bgp_conn_t *conn = sess->pri_conn;
    if (!conn || conn->fd < 0 || conn->state != BGP_CONN_STATE_ESTABLISHED)
    {
        return;
    }

    if (bgp_pkt_send_keepalive(conn) < 0)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BGP: Failed to send KEEPALIVE to %s, closing connection", addr_str);
        bgp_session_cancel_keepalive(sess, g_bgp_local->epoll_fd);
        bgp_session_cancel_hold(sess, g_bgp_local->epoll_fd);
        bgp_conn_close(&sess->pri_conn);
        (void)bgp_vrf_purge_session_routes(sess->vrf, &sess->neighbor_addr);
        bgp_arm_retry(sess);
    }
}

static void bgp_handle_hold_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->hold_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read hold timerfd");
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP: neighbor %s hold time expired, closing session", addr_str);

    bgp_session_cancel_keepalive(sess, g_bgp_local->epoll_fd);
    bgp_session_cancel_hold(sess, g_bgp_local->epoll_fd);
    bgp_conn_close(&sess->pri_conn);
    bgp_conn_close(&sess->sec_conn);
    (void)bgp_vrf_purge_session_routes(sess->vrf, &sess->neighbor_addr);
    bgp_arm_retry(sess);
}

static void bgp_handle_retry_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->retry_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read timerfd");
    }

    bgp_session_cancel_retry(sess, g_bgp_local->epoll_fd);

    if (sess->pri_conn)
    {
        return;
    }

    bgp_protocol_t *proto = g_bgp_local->protocol;
    bgp_vrf_t *vrf0 = proto ? bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID) : NULL;
    if (!vrf0 || !bgp_vrf_neighbor_has_any_af(vrf0, &sess->neighbor_addr))
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP: connect-retry expired, reconnecting neighbor %s", addr_str);
    bgp_server_start_active_conn(sess);
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

    while (g_bgp_local && g_bgp_local->running)
    {
        int nfds = epoll_wait(g_bgp_local->epoll_fd, events, BGP_MAX_EPOLL_EVENTS, 1000);

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
    if (!session || !g_bgp_local || g_bgp_local->epoll_fd < 0)
    {
        return;
    }

    if (session->pri_conn)
    {
        return;
    }

    bgp_conn_t *conn = bgp_conn_create(session);
    int fd = bgp_conn_start_active(conn, &session->neighbor_addr, g_bgp_local->epoll_fd);
    if (fd < 0)
    {
        char addr_str[64];
        net_addr_to_str(&session->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BGP: Failed to initiate active connection for neighbor %s, scheduling connect-retry", addr_str);
        bgp_conn_destroy(conn);
        bgp_arm_retry(session);
        return;
    }

    session->pri_conn = conn;
    (void)fd;
}

void bgp_server_stop_session_conns(bgp_session_t *session)
{
    if (!session || !g_bgp_local)
    {
        return;
    }
    bgp_session_cancel_retry(session, g_bgp_local->epoll_fd);
    bgp_session_cancel_keepalive(session, g_bgp_local->epoll_fd);
    bgp_session_cancel_hold(session, g_bgp_local->epoll_fd);
    bgp_conn_close(&session->pri_conn);
    bgp_conn_close(&session->sec_conn);
    (void)bgp_vrf_purge_session_routes(session->vrf, &session->neighbor_addr);
}

static void bgp_worker_runtime_cleanup(void)
{
    if (!g_bgp_local)
    {
        return;
    }

    if (g_bgp_local->protocol && g_bgp_local->protocol->vrf_hash)
    {
        GHashTableIter vrf_iter;
        gpointer vrf_key, vrf_val;
        g_hash_table_iter_init(&vrf_iter, g_bgp_local->protocol->vrf_hash);
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

    if (g_bgp_local->epoll_fd >= 0)
    {
        close(g_bgp_local->epoll_fd);
        g_bgp_local->epoll_fd = DEV_INVALID_FD;
        bgp_work_set_epoll_fd(DEV_INVALID_FD);
    }

    if (g_bgp_local->protocol)
    {
        bgp_protocol_destroy(g_bgp_local->protocol);
        g_bgp_local->protocol = NULL;
    }
}

static int bgp_worker_channel_init(void)
{
    if (!g_bgp_local)
    {
        return ERRCODE_FAIL;
    }

    if (!g_bgp_local->cmd_queue)
    {
        g_bgp_local->cmd_queue = g_async_queue_new();
        if (!g_bgp_local->cmd_queue)
        {
            LOG_ERROR("BGP: Failed to create command queue");
            return ERRCODE_FAIL;
        }
    }

    if (g_bgp_local->cmd_eventfd < 0)
    {
        g_bgp_local->cmd_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (g_bgp_local->cmd_eventfd < 0)
        {
            LOG_PERROR("BGP: Failed to create cmd eventfd");
            return ERRCODE_FAIL;
        }
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &bgp_cmd_tag;
    if (epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_ADD, g_bgp_local->cmd_eventfd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD cmd eventfd failed");
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

static void bgp_worker_channel_cleanup(void)
{
    if (!g_bgp_local)
    {
        return;
    }

    if (g_bgp_local->epoll_fd >= 0 && g_bgp_local->cmd_eventfd >= 0)
    {
        epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_local->cmd_eventfd, NULL);
    }

    if (g_bgp_local->cmd_eventfd >= 0)
    {
        close(g_bgp_local->cmd_eventfd);
        g_bgp_local->cmd_eventfd = -1;
    }

    bgp_worker_cmd_drain_queue();

    if (g_bgp_local->cmd_queue)
    {
        g_async_queue_unref(g_bgp_local->cmd_queue);
        g_bgp_local->cmd_queue = NULL;
    }
}

int bgp_worker_prepare(void)
{
    if (!g_bgp_local)
    {
        return ERRCODE_FAIL;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("BGP: Failed to create epoll");
        return ERRCODE_FAIL;
    }

    g_bgp_local->epoll_fd = epoll_fd;
    bgp_work_set_epoll_fd(epoll_fd);

    if (bgp_worker_channel_init() != ERRCODE_SUCCESS)
    {
        close(epoll_fd);
        g_bgp_local->epoll_fd = DEV_INVALID_FD;
        bgp_work_set_epoll_fd(DEV_INVALID_FD);
        bgp_worker_channel_cleanup();
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int bgp_worker_launch(void)
{
    if (!g_bgp_local)
    {
        return ERRCODE_FAIL;
    }

    g_bgp_local->running = 1;
    if (pthread_create(&g_bgp_local->worker_thread, NULL, bgp_worker_thread, NULL) != 0)
    {
        LOG_PERROR("BGP: Failed to create server thread");
        g_bgp_local->running = 0;
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
    if (!g_bgp_local)
    {
        return;
    }

    bgp_worker_cmd_t *shutdown_cmd = NULL;

    if (g_bgp_local->worker_thread)
    {
        shutdown_cmd = bgp_worker_cmd_create(BGP_WORKER_CMD_TYPE_SHUTDOWN, NULL, TRUE);
        if (shutdown_cmd && bgp_worker_cmd_enqueue(shutdown_cmd) == 0)
        {
            (void)bgp_worker_cmd_wait(shutdown_cmd);
        }
        else
        {
            g_bgp_local->running = 0;
        }

        pthread_join(g_bgp_local->worker_thread, NULL);
        g_bgp_local->worker_thread = 0;
    }
    else
    {
        bgp_worker_runtime_cleanup();
    }

    if (shutdown_cmd)
    {
        bgp_worker_cmd_destroy(shutdown_cmd);
    }

    g_bgp_local->running = 0;
    bgp_worker_channel_cleanup();
}
