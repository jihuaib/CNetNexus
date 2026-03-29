/**
 * @file   bgp_bmp.c
 * @brief  BGP BMP 客户端运行态：生命周期管理、连接管理、报文构建
 * @author jhb
 * @date   2026/03/29
 */
#include "bgp_bmp.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "bgp_worker.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 生命周期
// ============================================================================

bgp_bmp_instance_t *bgp_bmp_instance_create(const char *name)
{
    bgp_bmp_instance_t *inst = g_malloc0(sizeof(bgp_bmp_instance_t));
    if (!inst)
    {
        return NULL;
    }

    g_strlcpy(inst->name, name, sizeof(inst->name));
    inst->fd = -1;
    inst->reconnect_timerfd = -1;
    inst->stats_timerfd = -1;
    inst->conn_state = BGP_BMP_CONN_IDLE;
    inst->reconnect_interval = 30;
    inst->monitor_all = TRUE;

    /* 初始化 sentinel */
    inst->reconnect_sentinel.session = NULL;
    inst->reconnect_sentinel.type = BGP_TIMER_TYPE_BMP_RECONNECT;
    inst->stats_sentinel.session = NULL;
    inst->stats_sentinel.type = BGP_TIMER_TYPE_BMP_STATS;
    inst->conn_sentinel.session = NULL;
    inst->conn_sentinel.type = BGP_TIMER_TYPE_BMP_CONN;

    return inst;
}

void bgp_bmp_instance_destroy(bgp_bmp_instance_t *inst, int epoll_fd)
{
    if (!inst)
    {
        return;
    }

    /* 断开连接 */
    bgp_bmp_disconnect(inst, epoll_fd);

    /* 释放 monitor_peers 哈希表 */
    if (inst->monitor_peers)
    {
        g_hash_table_destroy(inst->monitor_peers);
        inst->monitor_peers = NULL;
    }

    g_free(inst);
}

gboolean bgp_bmp_should_monitor(const bgp_bmp_instance_t *inst, const char *peer_ip)
{
    if (!inst || !peer_ip)
    {
        return FALSE;
    }
    if (inst->monitor_all)
    {
        return TRUE;
    }
    if (!inst->monitor_peers)
    {
        return FALSE;
    }
    return g_hash_table_contains(inst->monitor_peers, peer_ip);
}

// ============================================================================
// 连接管理（Phase 3 实现）
// ============================================================================

void bgp_bmp_connect(bgp_bmp_instance_t *inst, int epoll_fd)
{
    /* TODO Phase 3: 实现 TCP 非阻塞连接到 collector */
    (void)inst;
    (void)epoll_fd;
}

void bgp_bmp_handle_connect_result(bgp_bmp_instance_t *inst, int epoll_fd)
{
    /* TODO Phase 3 */
    (void)inst;
    (void)epoll_fd;
}

void bgp_bmp_handle_read(bgp_bmp_instance_t *inst, int epoll_fd)
{
    /* TODO Phase 3 */
    (void)inst;
    (void)epoll_fd;
}

void bgp_bmp_handle_reconnect(bgp_bmp_instance_t *inst, int epoll_fd)
{
    /* TODO Phase 3 */
    (void)inst;
    (void)epoll_fd;
}

void bgp_bmp_disconnect(bgp_bmp_instance_t *inst, int epoll_fd)
{
    if (!inst)
    {
        return;
    }

    /* 关闭 TCP 连接 */
    if (inst->fd >= 0)
    {
        if (epoll_fd >= 0)
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, inst->fd, NULL);
        }
        close(inst->fd);
        inst->fd = -1;
    }

    /* 关闭重连定时器 */
    if (inst->reconnect_timerfd >= 0)
    {
        if (epoll_fd >= 0)
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, inst->reconnect_timerfd, NULL);
        }
        close(inst->reconnect_timerfd);
        inst->reconnect_timerfd = -1;
    }

    /* 关闭 stats 定时器 */
    if (inst->stats_timerfd >= 0)
    {
        if (epoll_fd >= 0)
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, inst->stats_timerfd, NULL);
        }
        close(inst->stats_timerfd);
        inst->stats_timerfd = -1;
    }

    inst->conn_state = BGP_BMP_CONN_IDLE;
    inst->connected_at_usec = 0;
}

// ============================================================================
// 报文发送（Phase 4-7 实现）
// ============================================================================

void bgp_bmp_send_initiation(bgp_bmp_instance_t *inst)
{
    /* TODO Phase 4 */
    (void)inst;
}

void bgp_bmp_send_termination(bgp_bmp_instance_t *inst)
{
    /* TODO Phase 4 */
    (void)inst;
}

void bgp_bmp_send_peer_up(bgp_bmp_instance_t *inst, bgp_session_t *sess)
{
    /* TODO Phase 5 */
    (void)inst;
    (void)sess;
}

void bgp_bmp_send_peer_down(bgp_bmp_instance_t *inst, bgp_session_t *sess, uint8_t reason)
{
    /* TODO Phase 5 */
    (void)inst;
    (void)sess;
    (void)reason;
}

void bgp_bmp_send_route_monitoring(bgp_bmp_instance_t *inst, bgp_session_t *sess, const uint8_t *bgp_pdu,
                                   uint16_t pdu_len)
{
    /* TODO Phase 6 */
    (void)inst;
    (void)sess;
    (void)bgp_pdu;
    (void)pdu_len;
}

void bgp_bmp_send_stats_report(bgp_bmp_instance_t *inst)
{
    /* TODO Phase 7 */
    (void)inst;
}

void bgp_bmp_handle_stats_timer(bgp_bmp_instance_t *inst)
{
    /* TODO Phase 7 */
    (void)inst;
}

// ============================================================================
// 事件钩子
// ============================================================================

void bgp_bmp_notify_peer_up(bgp_session_t *sess)
{
    if (!g_bgp_work_local || !g_bgp_work_local->bmp_instances)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, g_bgp_work_local->bmp_instances);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;
        if (inst->conn_state == BGP_BMP_CONN_UP && bgp_bmp_should_monitor(inst, addr_str))
        {
            bgp_bmp_send_peer_up(inst, sess);
        }
    }
}

void bgp_bmp_notify_peer_down(bgp_session_t *sess, uint8_t reason)
{
    if (!g_bgp_work_local || !g_bgp_work_local->bmp_instances)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, g_bgp_work_local->bmp_instances);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;
        if (inst->conn_state == BGP_BMP_CONN_UP && bgp_bmp_should_monitor(inst, addr_str))
        {
            bgp_bmp_send_peer_down(inst, sess, reason);
        }
    }
}

void bgp_bmp_notify_route_monitoring(bgp_session_t *sess, const uint8_t *bgp_pdu, uint16_t pdu_len)
{
    if (!g_bgp_work_local || !g_bgp_work_local->bmp_instances)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, g_bgp_work_local->bmp_instances);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;
        if (inst->conn_state == BGP_BMP_CONN_UP && bgp_bmp_should_monitor(inst, addr_str))
        {
            bgp_bmp_send_route_monitoring(inst, sess, bgp_pdu, pdu_len);
        }
    }
}

// ============================================================================
// 全局清理
// ============================================================================

void bgp_bmp_cleanup_all(int epoll_fd)
{
    if (!g_bgp_work_local || !g_bgp_work_local->bmp_instances)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, g_bgp_work_local->bmp_instances);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;
        bgp_bmp_instance_destroy(inst, epoll_fd);
    }

    g_hash_table_destroy(g_bgp_work_local->bmp_instances);
    g_bgp_work_local->bmp_instances = NULL;
}
