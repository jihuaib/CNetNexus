/**
 * @file   bgp_fsm.c
 * @brief  BGP 有限状态机（RFC 4271 §8）实现：基于函数指针分发表的状态迁移
 * @author jhb
 * @date   2026/03/21
 *
 * 设计说明：
 *   - 分发表 fsm_table[state][event] 存储每个 (state, event) 对对应的动作函数
 *   - NULL 条目表示该 (state, event) 组合在 RFC 中定义为"忽略"
 *   - bgp_fsm_event() 查表并调用对应动作，并在状态变化时记录日志
 *   - 动作函数通过 sess->pri_conn 和 g_bgp_work_local->epoll_fd 访问连接和 epoll
 *   - sec_conn 事件由调用方在调用 FSM 之前自行处理（关闭/提升），FSM 只操作 pri_conn
 */
#include "bgp_fsm.h"

#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "bgp.h"
#include "bgp_bmp.h"
#include "bgp_conn.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "db.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

/** 动作函数统一签名 */
typedef void (*bgp_fsm_action_fn)(bgp_session_t *sess);

/* 前向声明分发表（定义在文件末尾） */
static const bgp_fsm_action_fn fsm_table[BGP_FSM_STATE_MAX][BGP_EVT_MAX];

/** 内部辅助：获取 bgp worker 的 epoll fd */
static inline int fsm_epoll_fd(void)
{
    return g_bgp_work_local->epoll_fd;
}

// ============================================================================
// 名称查询（用于日志）
// ============================================================================

const char *bgp_fsm_state_str(bgp_fsm_state_t state)
{
    switch (state)
    {
        case BGP_FSM_STATE_IDLE:
            return "Idle";
        case BGP_FSM_STATE_CONNECT:
            return "Connect";
        case BGP_FSM_STATE_ACTIVE:
            return "Active";
        case BGP_FSM_STATE_OPEN_SENT:
            return "OpenSent";
        case BGP_FSM_STATE_OPEN_CONFIRM:
            return "OpenConfirm";
        case BGP_FSM_STATE_ESTABLISHED:
            return "Established";
        default:
            return "Unknown";
    }
}

const char *bgp_fsm_event_str(bgp_fsm_event_t evt)
{
    switch (evt)
    {
        case BGP_EVT_MANUAL_START:
            return "ManualStart(1)";
        case BGP_EVT_MANUAL_STOP:
            return "ManualStop(2)";
        case BGP_EVT_AUTO_START:
            return "AutomaticStart(3)";
        case BGP_EVT_AUTO_STOP:
            return "AutomaticStop(8)";
        case BGP_EVT_CONNECT_RETRY_EXPIRED:
            return "ConnectRetryTimer_Expires(9)";
        case BGP_EVT_HOLD_TIMER_EXPIRED:
            return "HoldTimer_Expires(10)";
        case BGP_EVT_KEEPALIVE_TIMER_EXPIRED:
            return "KeepaliveTimer_Expires(11)";
        case BGP_EVT_TCP_CR_ACKED:
            return "TcpCrAcked(14)";
        case BGP_EVT_TCP_CONNECTION_CONFIRMED:
            return "TcpConnectionConfirmed(15)";
        case BGP_EVT_TCP_CR_FATAL:
            return "TcpCrFatal(16)";
        case BGP_EVT_TCP_CONNECTION_FAILS:
            return "TcpConnectionFails(17)";
        case BGP_EVT_BGP_OPEN:
            return "BGPOpen(19)";
        case BGP_EVT_BGP_HEADER_ERR:
            return "BGPHeaderErr(21)";
        case BGP_EVT_BGP_OPEN_MSG_ERR:
            return "BGPOpenMsgErr(22)";
        case BGP_EVT_OPEN_COLLISION_DUMP:
            return "OpenCollisionDump(23)";
        case BGP_EVT_NOTIF_MSG_VER_ERR:
            return "NotifMsgVerErr(24)";
        case BGP_EVT_NOTIF_MSG:
            return "NotifMsg(25)";
        case BGP_EVT_KEEPALIVE_MSG:
            return "KeepAliveMsg(26)";
        case BGP_EVT_UPDATE_MSG:
            return "UpdateMsg(27)";
        case BGP_EVT_UPDATE_MSG_ERR:
            return "UpdateMsgErr(28)";
        default:
            return "Unknown";
    }
}

// ============================================================================
// FSM 内部辅助：低级操作原语
// ============================================================================

/** 从 epoll 移除 fd，延迟释放 bgp_conn_t，并将槽位置 NULL */
static void fsm_close_conn_slot(bgp_session_t *sess, bgp_conn_t **slot)
{
    if (!slot || !*slot)
    {
        return;
    }
    bgp_conn_close(sess, slot, fsm_epoll_fd());
}

/** 调度 ConnectRetry 定时器（从 VRF 配置读取间隔） */
static void fsm_arm_retry(bgp_session_t *sess)
{
    uint16_t sec =
        (sess->vrf && sess->vrf->connect_retry > 0) ? sess->vrf->connect_retry : BGP_TIMER_DEFAULT_CONNECT_RETRY;
    bgp_session_arm_retry(sess, fsm_epoll_fd(), sec);
}

static int fsm_prefix_contains_addr(const net_addr_t *prefix_addr, uint8_t prefix_len, const net_addr_t *addr)
{
    if (!prefix_addr || !addr || prefix_addr->family != addr->family)
    {
        return 0;
    }

    const uint8_t *pfx = NULL;
    const uint8_t *ip = NULL;
    uint8_t max_len = 0;

    if (addr->family == AF_INET)
    {
        pfx = (const uint8_t *)&prefix_addr->u.v4;
        ip = (const uint8_t *)&addr->u.v4;
        max_len = 32U;
    }
    else if (addr->family == AF_INET6)
    {
        pfx = (const uint8_t *)&prefix_addr->u.v6;
        ip = (const uint8_t *)&addr->u.v6;
        max_len = 128U;
    }
    else
    {
        return 0;
    }

    if (prefix_len == 0 || prefix_len > max_len)
    {
        return 0;
    }

    uint8_t full_bytes = (uint8_t)(prefix_len / 8U);
    uint8_t rem_bits = (uint8_t)(prefix_len % 8U);
    if (full_bytes > 0 && memcmp(pfx, ip, full_bytes) != 0)
    {
        return 0;
    }
    if (rem_bits > 0)
    {
        uint8_t mask = (uint8_t)(0xFFU << (8U - rem_bits));
        if ((pfx[full_bytes] & mask) != (ip[full_bytes] & mask))
        {
            return 0;
        }
    }
    return 1;
}

static gboolean fsm_neighbor_is_directly_connected(const net_addr_t *neighbor_addr)
{
    if (!neighbor_addr || neighbor_addr->family == 0)
    {
        return FALSE;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(g_bgp_local->dev_ipc_ctx, "if_interface", NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        db_result_free(result);
        return FALSE;
    }

    gboolean direct = FALSE;
    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        const db_row_t *row = result->rows[i];
        const char *ip_str = (neighbor_addr->family == AF_INET6) ? db_row_get_text(row, "ipv6_address", NULL)
                                                                 : db_row_get_text(row, "ip_address", NULL);
        int64_t prefix_len = (neighbor_addr->family == AF_INET6) ? db_row_get_int(row, "ipv6_prefix_len", 0)
                                                                 : db_row_get_int(row, "prefix_len", 0);
        int64_t shutdown = db_row_get_int(row, "shutdown", 0);
        if (!ip_str || ip_str[0] == '\0' || shutdown != 0)
        {
            continue;
        }

        net_addr_t local_addr;
        if (net_addr_from_str(ip_str, &local_addr) != 0 || local_addr.family != neighbor_addr->family)
        {
            continue;
        }
        if (prefix_len <= 0 || prefix_len > ((neighbor_addr->family == AF_INET) ? 32 : 128))
        {
            continue;
        }

        if (fsm_prefix_contains_addr(&local_addr, (uint8_t)prefix_len, neighbor_addr))
        {
            direct = TRUE;
            break;
        }
    }

    db_result_free(result);
    return direct;
}

/** 向对端发送 BGP OPEN 报文（通过 sess->pri_conn） */
static void fsm_send_open(bgp_session_t *sess)
{
    bgp_conn_t *conn = sess->pri_conn;
    if (!conn)
    {
        return;
    }
    bgp_protocol_t *proto = g_bgp_work_local->protocol;
    bgp_vrf_t *vrf0 = proto ? bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID) : NULL;
    GList *af_peers = vrf0 ? bgp_vrf_get_session_peers(vrf0, &sess->neighbor_addr) : NULL;
    bgp_pkt_send_open(conn, proto ? proto->as_number : 0, vrf0 ? vrf0->router_id : 0, af_peers);
    g_list_free(af_peers);
}

/**
 * @brief 关闭 pri_conn，必要时将 sec_conn 提升为 pri_conn
 *
 * 若提升成功，将 session 视角切回新 pri_conn：TCP 未完成则 Connect，否则 OpenSent。
 * 若两条连接均已关闭：purge_routes 时清除路由；arm_retry 时进入 Active，否则 Idle。
 */
static void fsm_close_primary(bgp_session_t *sess, gboolean purge_routes, gboolean arm_retry)
{
    /* BMP Peer Down 通知（ESTABLISHED → 连接关闭，且无 sec_conn 可提升时） */
    if (sess->fsm_state == BGP_FSM_STATE_ESTABLISHED && !sess->sec_conn)
    {
        bgp_bmp_notify_peer_down(sess, BGP_BMP_PEER_DOWN_REMOTE_NO_NOTIFY);
    }

    if (sess->pri_conn)
    {
        sess->pri_last_socket_error = sess->pri_conn->last_socket_error;
    }
    fsm_close_conn_slot(sess, &sess->pri_conn);

    /* 尝试将 sec_conn 提升为 pri_conn */
    if (sess->sec_conn)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP FSM: neighbor=%s sec_conn fd=%d promoted to pri_conn", addr_str, sess->sec_conn->fd);
        sess->pri_conn = sess->sec_conn;
        sess->sec_conn = NULL;
        sess->pri_last_socket_error = sess->pri_conn->last_socket_error;
        sess->sec_last_socket_error = 0;
        sess->fsm_state = sess->pri_conn->is_connecting ? BGP_FSM_STATE_CONNECT : BGP_FSM_STATE_OPEN_SENT;
        return;
    }

    /* 两条连接均已关闭 */
    if (purge_routes && sess->vrf)
    {
        (void)bgp_vrf_purge_session_routes(sess->vrf, &sess->neighbor_addr);
    }
    bgp_session_reset_negotiated(sess);

    if (arm_retry)
    {
        fsm_arm_retry(sess);
        sess->fsm_state = BGP_FSM_STATE_ACTIVE;
    }
    else
    {
        sess->fsm_state = BGP_FSM_STATE_IDLE;
    }
}

/** 关闭全部连接和定时器；arm_retry 时进入 Active，否则 Idle */
static void fsm_close_all(bgp_session_t *sess, gboolean purge_routes, gboolean arm_retry)
{
    /* BMP Peer Down 通知（从 ESTABLISHED 离开时） */
    if (sess->fsm_state == BGP_FSM_STATE_ESTABLISHED)
    {
        bgp_bmp_notify_peer_down(sess, BGP_BMP_PEER_DOWN_REMOTE_NO_NOTIFY);
    }

    if (sess->pri_conn)
    {
        sess->pri_last_socket_error = sess->pri_conn->last_socket_error;
    }
    if (sess->sec_conn)
    {
        sess->sec_last_socket_error = sess->sec_conn->last_socket_error;
    }
    bgp_session_cancel_keepalive(sess, fsm_epoll_fd());
    bgp_session_cancel_hold(sess, fsm_epoll_fd());
    bgp_session_cancel_retry(sess, fsm_epoll_fd());
    fsm_close_conn_slot(sess, &sess->pri_conn);
    fsm_close_conn_slot(sess, &sess->sec_conn);
    if (purge_routes && sess->vrf)
    {
        (void)bgp_vrf_purge_session_routes(sess->vrf, &sess->neighbor_addr);
    }
    bgp_session_reset_negotiated(sess);
    if (arm_retry)
    {
        fsm_arm_retry(sess);
        sess->fsm_state = BGP_FSM_STATE_ACTIVE;
    }
    else
    {
        sess->fsm_state = BGP_FSM_STATE_IDLE;
    }
}

/** 进入 ESTABLISHED 后，将当前所有 AF 的 best-route 快照挂入 session pub_queue */
static void fsm_reannounce_best(bgp_session_t *sess)
{
    bgp_conn_t *conn = sess->pri_conn;
    if (!conn || conn->fd < 0 || !sess->vrf)
    {
        return;
    }
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP FSM: neighbor=%s Established via fd=%d (%s), scheduling best-route replay (af_peers=%u)", addr_str,
             conn->fd, conn->is_active ? "active" : "passive", (unsigned)g_list_length(sess->peer_list));
    bgp_work_enqueue_best_for_session(sess);
}

/** 进入 ESTABLISHED 状态的统一动作：记录时间戳、启动 KA/Hold 定时器、补发路由 */
static void fsm_on_established(bgp_session_t *sess)
{
    sess->established_at_usec = g_get_real_time();
    sess->fsm_state = BGP_FSM_STATE_ESTABLISHED;

    bgp_protocol_t *proto = g_bgp_work_local->protocol;
    bgp_vrf_t *vrf0 = proto ? bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID) : NULL;
    uint16_t ka_sec = vrf0 ? vrf0->keepalive : BGP_TIMER_DEFAULT_KEEPALIVE;
    bgp_session_arm_keepalive(sess, fsm_epoll_fd(), ka_sec);
    if (sess->negotiated_hold > 0)
    {
        bgp_session_arm_hold(sess, fsm_epoll_fd(), sess->negotiated_hold);
    }
    fsm_reannounce_best(sess);

    /* BMP Peer Up 通知 */
    bgp_bmp_notify_peer_up(sess);
}

// ============================================================================
// 动作函数（供分发表引用）
// ============================================================================

/* ---- 通用动作 ---- */

/** 记录警告并忽略（表中 NULL 的补充，用于需要提示的意外事件） */
static void act_warn_ignore(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s [%s] received unexpected event, ignored", addr_str,
             bgp_fsm_state_str(sess->fsm_state));
}

/* ---- Idle / Active 共用：启动主动连接 ---- */

/**
 * @brief Event 1/3（ManualStart/AutoStart）in Idle/Active：发起主动 TCP 连接
 *
 * 成功 → Connect；失败（socket 错误）→ 调度 ConnectRetry → Active。
 */
static void act_start_active(bgp_session_t *sess)
{
    if (sess->pri_conn)
    {
        return; /* 已有连接，幂等 */
    }
    bgp_conn_t *new_conn = bgp_conn_create(sess);

    bgp_protocol_t *proto = g_bgp_work_local->protocol;
    gboolean is_ebgp = (proto && proto->as_number != 0 && sess->remote_as != proto->as_number) ? TRUE : FALSE;
    if (is_ebgp)
    {
        if (sess->ebgp_multihop_ttl > 0)
        {
            new_conn->has_ttl = TRUE;
            new_conn->ttl = sess->ebgp_multihop_ttl;
        }
        else if (fsm_neighbor_is_directly_connected(&sess->neighbor_addr))
        {
            new_conn->has_ttl = TRUE;
            new_conn->ttl = 1;
        }
        else
        {
            char nbr[64];
            net_addr_to_str(&sess->neighbor_addr, nbr, sizeof(nbr));
            LOG_WARN(
                "BGP FSM: neighbor=%s eBGP peer is not directly connected; configure 'neighbor %s ebgp-multihop <ttl>'",
                nbr, nbr);
            bgp_conn_destroy(new_conn);
            fsm_arm_retry(sess);
            sess->fsm_state = BGP_FSM_STATE_ACTIVE;
            return;
        }
    }

    if (sess->source_addr.family != 0)
    {
        new_conn->has_local_addr = TRUE;
        new_conn->local_addr = sess->source_addr;
    }
    if (bgp_conn_start_active(new_conn, &sess->neighbor_addr, fsm_epoll_fd()) < 0)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BGP FSM: neighbor=%s failed to initiate TCP, scheduling ConnectRetry", addr_str);
        bgp_conn_destroy(new_conn);
        fsm_arm_retry(sess);
        sess->fsm_state = BGP_FSM_STATE_ACTIVE;
        return;
    }
    sess->pri_conn = new_conn;
    sess->pri_last_socket_error = 0;
    sess->fsm_state = BGP_FSM_STATE_CONNECT;
}

/* ---- Idle：Event 15（TcpConnectionConfirmed）---- */

/**
 * @brief 被动入站（Idle 状态）：直接发送 OPEN → OpenSent
 *
 * 调用方已将 conn 挂入 sess->pri_conn。
 */
static void act_idle_tcp_confirmed(bgp_session_t *sess)
{
    fsm_send_open(sess);
    sess->fsm_state = BGP_FSM_STATE_OPEN_SENT;
}

/* ---- Connect 状态动作 ---- */

/**
 * @brief Event 2/8（Stop）in Connect：取消所有资源，回到 Idle
 */
static void act_connect_stop(bgp_session_t *sess)
{
    fsm_close_all(sess, TRUE, FALSE);
}

/**
 * @brief Event 14（TcpCrAcked）in Connect：主动 TCP 握手成功，发送 OPEN → OpenSent
 *
 * epoll 已由 bgp_handle_active_connect 改为 EPOLLIN，conn->is_connecting 已清除。
 */
static void act_connect_tcp_acked(bgp_session_t *sess)
{
    fsm_send_open(sess);
    sess->fsm_state = BGP_FSM_STATE_OPEN_SENT;
}

/**
 * @brief Event 16/17（TcpCrFatal/TcpConnectionFails）in Connect：主动 TCP 失败
 *
 * 关闭 pri_conn；若有 sec_conn 则提升（FSM 跟随 sec_conn 状态）；否则调度重试 → Active。
 */
static void act_connect_tcp_fails(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    bgp_conn_t *conn = sess->pri_conn;
    if (conn && conn->last_socket_error != 0)
    {
        LOG_WARN("BGP FSM: neighbor=%s Connect: TCP failed (errno=%d, %s), reconnecting", addr_str,
                 conn->last_socket_error, strerror(conn->last_socket_error));
    }
    else
    {
        LOG_WARN("BGP FSM: neighbor=%s Connect: TCP failed, reconnecting", addr_str);
    }
    bgp_session_cancel_keepalive(sess, fsm_epoll_fd());
    bgp_session_cancel_hold(sess, fsm_epoll_fd());
    fsm_close_primary(sess, FALSE, TRUE);
}

/**
 * @brief Event 9（ConnectRetryTimer_Expires）in Connect：重新发起连接
 */
static void act_connect_retry_expired(bgp_session_t *sess)
{
    bgp_session_cancel_retry(sess, fsm_epoll_fd());
    act_start_active(sess);
}

/* ---- Active 状态动作 ---- */

/**
 * @brief Event 2/8（Stop）in Active：取消重试定时器 → Idle
 */
static void act_active_stop(bgp_session_t *sess)
{
    bgp_session_cancel_retry(sess, fsm_epoll_fd());
    sess->fsm_state = BGP_FSM_STATE_IDLE;
}

/**
 * @brief Event 9（ConnectRetryTimer_Expires）in Active：取消旧定时器，发起新连接
 */
static void act_active_retry_expired(bgp_session_t *sess)
{
    bgp_session_cancel_retry(sess, fsm_epoll_fd());
    act_start_active(sess);
}

/**
 * @brief Event 15（TcpConnectionConfirmed）in Active：被动入站
 *
 * 取消重试定时器，向新连接发送 OPEN → OpenSent。
 * 调用方已将 conn 挂入 sess->pri_conn。
 */
static void act_active_tcp_confirmed(bgp_session_t *sess)
{
    bgp_session_cancel_retry(sess, fsm_epoll_fd());
    fsm_send_open(sess);
    sess->fsm_state = BGP_FSM_STATE_OPEN_SENT;
}

/* ---- OpenSent/OpenConfirm 共用动作 ---- */

/**
 * @brief Event 2/8（Stop）in OpenSent/OpenConfirm：发送 NOTIFICATION Cease → Idle
 */
static void act_open_stop(bgp_session_t *sess)
{
    bgp_conn_t *conn = sess->pri_conn;
    if (conn && conn->fd >= 0 && !conn->is_connecting)
    {
        bgp_pkt_send_notification(conn, BGP_ERR_CEASE, BGP_CEASE_ADMIN_RESET);
    }
    fsm_close_all(sess, TRUE, FALSE);
}

/**
 * @brief Event 16/17（TCP 失败）in OpenSent/OpenConfirm：关闭当前连接 → Active
 */
static void act_open_tcp_fails(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s [%s]: TCP lost, reconnecting", addr_str, bgp_fsm_state_str(sess->fsm_state));
    fsm_close_primary(sess, TRUE, TRUE);
}

/**
 * @brief Event 10（HoldTimer_Expires）in OpenSent/OpenConfirm：NOTIFICATION Hold Timer → Active
 */
static void act_open_hold_expired(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s [%s]: HoldTimer expired", addr_str, bgp_fsm_state_str(sess->fsm_state));
    bgp_conn_t *conn = sess->pri_conn;
    if (conn && conn->fd >= 0)
    {
        bgp_pkt_send_notification(conn, BGP_ERR_HOLD_TIMER, 0);
    }
    fsm_close_primary(sess, TRUE, TRUE);
}

/**
 * @brief Event 25（NotifMsg）in OpenSent/OpenConfirm：收到 NOTIFICATION → Active
 */
static void act_open_notif(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s [%s]: received NOTIFICATION, reconnecting", addr_str,
             bgp_fsm_state_str(sess->fsm_state));
    fsm_close_primary(sess, TRUE, TRUE);
}

/**
 * @brief Event 24（NotifMsgVerErr）in OpenSent/OpenConfirm：版本错误 NOTIFICATION → Idle（不重试）
 */
static void act_open_notif_ver_err(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s [%s]: version error NOTIFICATION, closing without retry", addr_str,
             bgp_fsm_state_str(sess->fsm_state));
    fsm_close_primary(sess, TRUE, FALSE);
}

/**
 * @brief Event 21/22（BGPHeaderErr/BGPOpenMsgErr）in OpenSent/OpenConfirm：报文错误 → Active
 */
static void act_open_bgp_err(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s [%s]: BGP message error", addr_str, bgp_fsm_state_str(sess->fsm_state));
    fsm_close_primary(sess, TRUE, TRUE);
}

/**
 * @brief Event 23（OpenCollisionDump）in OpenSent/OpenConfirm：RFC §6.8 碰撞—关闭 pri_conn
 */
static void act_collision_dump(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP FSM: neighbor=%s [%s]: §6.8 collision dump pri_conn", addr_str, bgp_fsm_state_str(sess->fsm_state));
    fsm_close_primary(sess, FALSE, TRUE);
}

/**
 * @brief 未预期事件的默认处理（RFC §8.2.2：发 NOTIFICATION FSM-Error → Active）
 *
 * 用于 OpenSent/OpenConfirm 中表中未覆盖的事件。
 */
static void act_open_fsm_error(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s [%s]: unexpected event, sending FSM-Error NOTIFICATION", addr_str,
             bgp_fsm_state_str(sess->fsm_state));
    bgp_conn_t *conn = sess->pri_conn;
    if (conn && conn->fd >= 0 && !conn->is_connecting)
    {
        bgp_pkt_send_notification(conn, BGP_ERR_FSM, 0);
    }
    fsm_close_primary(sess, TRUE, TRUE);
}

/* ---- OpenSent 专用动作 ---- */

/**
 * @brief Event 19（BGPOpen）in OpenSent：收到合法 OPEN → OpenConfirm
 *
 * bgp_pkt.c 已发送 KEEPALIVE；FSM 只需推进 session 到 OpenConfirm。
 */
static void act_open_sent_bgp_open(bgp_session_t *sess)
{
    sess->fsm_state = BGP_FSM_STATE_OPEN_CONFIRM;
}

/* ---- OpenConfirm 专用动作 ---- */

/**
 * @brief Event 26（KeepAliveMsg）in OpenConfirm：收到 KEEPALIVE → Established
 *
 * bgp_pkt.c 已完成握手 KEEPALIVE 接收；FSM 只需进入 Established。
 * FSM 取消重试定时器，启动 KA/Hold 定时器，补发 best-route。
 */
static void act_open_confirm_keepalive(bgp_session_t *sess)
{
    bgp_session_cancel_retry(sess, fsm_epoll_fd());
    fsm_on_established(sess);
}

/* ---- Established 状态动作 ---- */

/**
 * @brief Event 2/8（Stop）in Established：发送 NOTIFICATION Cease → Idle（不重连）
 */
static void act_estab_stop(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP FSM: neighbor=%s Established: admin stop", addr_str);
    bgp_conn_t *conn = sess->pri_conn;
    if (conn && conn->fd >= 0)
    {
        bgp_pkt_send_notification(conn, BGP_ERR_CEASE, BGP_CEASE_ADMIN_RESET);
    }
    fsm_close_all(sess, TRUE, FALSE);
}

/**
 * @brief Event 11（KeepaliveTimer_Expires）in Established：发送 KEEPALIVE
 *
 * 若发送失败，关闭会话并调度重试 → Active。
 */
static void act_estab_ka_timer(bgp_session_t *sess)
{
    bgp_conn_t *conn = sess->pri_conn;
    if (!conn || conn->fd < 0)
    {
        return;
    }
    if (bgp_pkt_send_keepalive(conn) < 0)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BGP FSM: neighbor=%s Established: KEEPALIVE send failed, closing", addr_str);
        bgp_session_cancel_keepalive(sess, fsm_epoll_fd());
        bgp_session_cancel_hold(sess, fsm_epoll_fd());
        fsm_close_all(sess, TRUE, TRUE);
    }
}

/**
 * @brief Event 10（HoldTimer_Expires）in Established：NOTIFICATION Hold Timer → Active
 */
static void act_estab_hold_expired(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s Established: HoldTimer expired", addr_str);
    bgp_conn_t *conn = sess->pri_conn;
    if (conn && conn->fd >= 0)
    {
        bgp_pkt_send_notification(conn, BGP_ERR_HOLD_TIMER, 0);
    }
    bgp_session_cancel_keepalive(sess, fsm_epoll_fd());
    bgp_session_cancel_hold(sess, fsm_epoll_fd());
    fsm_close_all(sess, TRUE, TRUE);
}

/**
 * @brief Event 25（NotifMsg）in Established：收到 NOTIFICATION → Active
 */
static void act_estab_notif(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s Established: received NOTIFICATION, reconnecting", addr_str);
    bgp_session_cancel_keepalive(sess, fsm_epoll_fd());
    bgp_session_cancel_hold(sess, fsm_epoll_fd());
    fsm_close_all(sess, TRUE, TRUE);
}

/**
 * @brief Event 28（UpdateMsgErr）in Established：NOTIFICATION UPDATE-Error → Active
 */
static void act_estab_update_err(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s Established: UPDATE error, sending NOTIFICATION", addr_str);
    bgp_conn_t *conn = sess->pri_conn;
    if (conn && conn->fd >= 0)
    {
        bgp_pkt_send_notification(conn, BGP_ERR_UPDATE, 0);
    }
    bgp_session_cancel_keepalive(sess, fsm_epoll_fd());
    bgp_session_cancel_hold(sess, fsm_epoll_fd());
    fsm_close_all(sess, TRUE, TRUE);
}

/**
 * @brief 未预期事件的默认处理（Established）：发 NOTIFICATION FSM-Error → Active
 */
static void act_estab_fsm_error(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP FSM: neighbor=%s Established: unexpected event, sending FSM-Error NOTIFICATION", addr_str);
    bgp_conn_t *conn = sess->pri_conn;
    if (conn && conn->fd >= 0)
    {
        bgp_pkt_send_notification(conn, BGP_ERR_FSM, 0);
    }
    bgp_session_cancel_keepalive(sess, fsm_epoll_fd());
    bgp_session_cancel_hold(sess, fsm_epoll_fd());
    fsm_close_all(sess, TRUE, TRUE);
}

// ============================================================================
// 分发表（RFC 4271 §8.2.2 Table 8 对应实现）
//
// 索引：fsm_table[bgp_fsm_state_t][bgp_fsm_event_t]
// NULL 条目：RFC 定义为"忽略"的 (state, event) 组合（无动作）
// ============================================================================

static const bgp_fsm_action_fn fsm_table[BGP_FSM_STATE_MAX][BGP_EVT_MAX] = {

    /* ----- Idle (0) ----- */
    [BGP_FSM_STATE_IDLE] =
        {
            [BGP_EVT_MANUAL_START] = act_start_active,                   /* Event 1 */
            [BGP_EVT_AUTO_START] = act_start_active,                     /* Event 3 */
            [BGP_EVT_TCP_CONNECTION_CONFIRMED] = act_idle_tcp_confirmed, /* Event 15 */
            /* Event 2/8 在 Idle 无意义，RFC 规定忽略（NULL） */
        },

    /* ----- Connect (1) ----- */
    [BGP_FSM_STATE_CONNECT] =
        {
            [BGP_EVT_MANUAL_STOP] = act_connect_stop,                    /* Event 2 */
            [BGP_EVT_AUTO_STOP] = act_connect_stop,                      /* Event 8 */
            [BGP_EVT_CONNECT_RETRY_EXPIRED] = act_connect_retry_expired, /* Event 9 */
            [BGP_EVT_TCP_CR_ACKED] = act_connect_tcp_acked,              /* Event 14 */
            [BGP_EVT_TCP_CR_FATAL] = act_connect_tcp_fails,              /* Event 16 */
            [BGP_EVT_TCP_CONNECTION_FAILS] = act_connect_tcp_fails,      /* Event 17 */
            /* Event 15 (TcpConnectionConfirmed) 碰撞由调用方处理 sec_conn，不经 FSM */
        },

    /* ----- Active (2) ----- */
    [BGP_FSM_STATE_ACTIVE] =
        {
            [BGP_EVT_MANUAL_STOP] = act_active_stop,                       /* Event 2 */
            [BGP_EVT_AUTO_STOP] = act_active_stop,                         /* Event 8 */
            [BGP_EVT_CONNECT_RETRY_EXPIRED] = act_active_retry_expired,    /* Event 9 */
            [BGP_EVT_TCP_CONNECTION_CONFIRMED] = act_active_tcp_confirmed, /* Event 15 */
        },

    /* ----- OpenSent (3) ----- */
    [BGP_FSM_STATE_OPEN_SENT] =
        {
            [BGP_EVT_MANUAL_STOP] = act_open_stop,                /* Event 2 */
            [BGP_EVT_AUTO_STOP] = act_open_stop,                  /* Event 8 */
            [BGP_EVT_HOLD_TIMER_EXPIRED] = act_open_hold_expired, /* Event 10 */
            [BGP_EVT_TCP_CR_FATAL] = act_open_tcp_fails,          /* Event 16 */
            [BGP_EVT_TCP_CONNECTION_FAILS] = act_open_tcp_fails,  /* Event 17 */
            [BGP_EVT_BGP_OPEN] = act_open_sent_bgp_open,          /* Event 19 */
            [BGP_EVT_BGP_HEADER_ERR] = act_open_bgp_err,          /* Event 21 */
            [BGP_EVT_BGP_OPEN_MSG_ERR] = act_open_bgp_err,        /* Event 22 */
            [BGP_EVT_OPEN_COLLISION_DUMP] = act_collision_dump,   /* Event 23 */
            [BGP_EVT_NOTIF_MSG_VER_ERR] = act_open_notif_ver_err, /* Event 24 */
            [BGP_EVT_NOTIF_MSG] = act_open_notif,                 /* Event 25 */
            /* Event 15 碰撞由调用方处理 sec_conn，不经 FSM */
        },

    /* ----- OpenConfirm (4) ----- */
    [BGP_FSM_STATE_OPEN_CONFIRM] =
        {
            [BGP_EVT_MANUAL_STOP] = act_open_stop,                /* Event 2 */
            [BGP_EVT_AUTO_STOP] = act_open_stop,                  /* Event 8 */
            [BGP_EVT_HOLD_TIMER_EXPIRED] = act_open_hold_expired, /* Event 10 */
            [BGP_EVT_TCP_CR_FATAL] = act_open_tcp_fails,          /* Event 16 */
            [BGP_EVT_TCP_CONNECTION_FAILS] = act_open_tcp_fails,  /* Event 17 */
            [BGP_EVT_BGP_HEADER_ERR] = act_open_bgp_err,          /* Event 21 */
            [BGP_EVT_BGP_OPEN_MSG_ERR] = act_open_bgp_err,        /* Event 22 */
            [BGP_EVT_OPEN_COLLISION_DUMP] = act_collision_dump,   /* Event 23 */
            [BGP_EVT_NOTIF_MSG_VER_ERR] = act_open_notif_ver_err, /* Event 24 */
            [BGP_EVT_NOTIF_MSG] = act_open_notif,                 /* Event 25 */
            [BGP_EVT_KEEPALIVE_MSG] = act_open_confirm_keepalive, /* Event 26 */
            /* Event 15 碰撞由调用方处理 sec_conn，不经 FSM */
        },

    /* ----- Established (5) ----- */
    [BGP_FSM_STATE_ESTABLISHED] =
        {
            [BGP_EVT_MANUAL_STOP] = act_estab_stop,                 /* Event 2 */
            [BGP_EVT_AUTO_STOP] = act_estab_stop,                   /* Event 8 */
            [BGP_EVT_KEEPALIVE_TIMER_EXPIRED] = act_estab_ka_timer, /* Event 11 */
            [BGP_EVT_HOLD_TIMER_EXPIRED] = act_estab_hold_expired,  /* Event 10 */
            [BGP_EVT_NOTIF_MSG] = act_estab_notif,                  /* Event 25 */
            [BGP_EVT_UPDATE_MSG_ERR] = act_estab_update_err,        /* Event 28 */
            /* Event 26/27（KA/UPDATE）在 Established 状态 hold_reset 由调用方处理（NULL）*/
        },
};

// ============================================================================
// 统一事件入口
// ============================================================================

void bgp_fsm_event(bgp_session_t *sess, bgp_fsm_event_t evt)
{
    if (!sess)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    /* 越界检查：防止访问表外内存 */
    if ((unsigned)sess->fsm_state >= BGP_FSM_STATE_MAX || (unsigned)evt >= BGP_EVT_MAX)
    {
        LOG_ERROR("BGP FSM: neighbor=%s invalid state=%d or event=%d", addr_str, (int)sess->fsm_state, (int)evt);
        return;
    }

    bgp_fsm_state_t old_state = sess->fsm_state;
    LOG_DEBUG("BGP FSM: neighbor=%s [%s] + %s", addr_str, bgp_fsm_state_str(old_state), bgp_fsm_event_str(evt));

    /* 查表：获取动作函数 */
    bgp_fsm_action_fn action = fsm_table[sess->fsm_state][evt];

    if (action)
    {
        action(sess);
    }
    else
    {
        /* 表中 NULL 条目：根据当前状态选择默认处理 */
        if (sess->fsm_state == BGP_FSM_STATE_OPEN_SENT || sess->fsm_state == BGP_FSM_STATE_OPEN_CONFIRM)
        {
            act_open_fsm_error(sess);
        }
        else if (sess->fsm_state == BGP_FSM_STATE_ESTABLISHED)
        {
            act_estab_fsm_error(sess);
        }
        else
        {
            act_warn_ignore(sess);
        }
    }

    /* 记录状态迁移日志 */
    if (old_state != sess->fsm_state)
    {
        LOG_INFO("BGP FSM: neighbor=%s %s → %s (event=%s)", addr_str, bgp_fsm_state_str(old_state),
                 bgp_fsm_state_str(sess->fsm_state), bgp_fsm_event_str(evt));
    }
}
