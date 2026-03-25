/**
 * @file   sbmp_main.c
 * @brief  SBMP（BMP Server）模块主入口，三阶段初始化、BMP 接入与运行态维护
 * @author jhb
 * @date   2026/03/08
 */
#include "sbmp_main.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bgp.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "sbmp_bdr.h"
#include "sbmp_bmp.h"
#include "sbmp_cli.h"
#include "sbmp_db.h"

/** SBMP 模块全局状态 */
sbmp_local_t *g_sbmp_local = NULL;

/** server epoll 单次最大事件数 */
#define SBMP_MAX_EPOLL_EVENTS 32

// ============================================================================
// 内部辅助：生命周期与查找
// ============================================================================

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
    {
        return -1;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void sbmp_peer_destroy(sbmp_peer_t *peer)
{
    g_free(peer);
}

static sbmp_client_t *sbmp_client_create(const char *client_id, const char *ip, uint16_t port, int fd)
{
    sbmp_client_t *c = g_malloc0(sizeof(sbmp_client_t));
    snprintf(c->client_id, sizeof(c->client_id), "%s", client_id ? client_id : "");
    snprintf(c->source_ip, sizeof(c->source_ip), "%s", ip ? ip : "");
    c->source_port = port;
    c->fd = fd;
    c->connected = (fd >= 0);
    c->connected_at_usec = g_get_real_time();
    c->last_active_usec = c->connected_at_usec;
    c->peer_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)sbmp_peer_destroy);
    c->rib_v4u_pre = sbmp_rib_create();
    c->rib_v4u_post = sbmp_rib_create();
    c->rib_v4u_loc_rib = sbmp_rib_create();
    c->rib_v6u_pre = sbmp_rib_create();
    c->rib_v6u_post = sbmp_rib_create();
    c->rib_v6u_loc_rib = sbmp_rib_create();
    return c;
}

static void sbmp_client_destroy(sbmp_client_t *client)
{
    if (!client)
    {
        return;
    }

    if (client->fd >= 0)
    {
        close(client->fd);
        client->fd = -1;
    }
    if (client->peer_hash)
    {
        g_hash_table_destroy(client->peer_hash);
        client->peer_hash = NULL;
    }
    if (client->rib_v4u_pre)
    {
        sbmp_rib_destroy(client->rib_v4u_pre);
        client->rib_v4u_pre = NULL;
    }
    if (client->rib_v4u_post)
    {
        sbmp_rib_destroy(client->rib_v4u_post);
        client->rib_v4u_post = NULL;
    }
    if (client->rib_v4u_loc_rib)
    {
        sbmp_rib_destroy(client->rib_v4u_loc_rib);
        client->rib_v4u_loc_rib = NULL;
    }
    if (client->rib_v6u_pre)
    {
        sbmp_rib_destroy(client->rib_v6u_pre);
        client->rib_v6u_pre = NULL;
    }
    if (client->rib_v6u_post)
    {
        sbmp_rib_destroy(client->rib_v6u_post);
        client->rib_v6u_post = NULL;
    }
    if (client->rib_v6u_loc_rib)
    {
        sbmp_rib_destroy(client->rib_v6u_loc_rib);
        client->rib_v6u_loc_rib = NULL;
    }

    g_free(client);
}

static sbmp_peer_t *sbmp_client_get_or_create_peer(sbmp_client_t *client, const char *peer_ip)
{
    sbmp_peer_t *peer = g_hash_table_lookup(client->peer_hash, peer_ip);
    if (peer)
    {
        return peer;
    }

    peer = g_malloc0(sizeof(sbmp_peer_t));
    snprintf(peer->peer_ip, sizeof(peer->peer_ip), "%s", peer_ip);
    g_hash_table_insert(client->peer_hash, g_strdup(peer_ip), peer);
    return peer;
}

static sbmp_rib_t *sbmp_client_get_rib(sbmp_client_t *client, uint16_t afi, uint8_t safi, sbmp_route_policy_t policy)
{
    if (!client || safi != BGP_SAFI_UNICAST)
    {
        return NULL;
    }

    if (afi == BGP_AFI_IPV4)
    {
        if (policy == SBMP_ROUTE_POLICY_POST)
        {
            return client->rib_v4u_post;
        }
        if (policy == SBMP_ROUTE_POLICY_LOC_RIB)
        {
            return client->rib_v4u_loc_rib;
        }
        return client->rib_v4u_pre;
    }

    if (afi == BGP_AFI_IPV6)
    {
        if (policy == SBMP_ROUTE_POLICY_POST)
        {
            return client->rib_v6u_post;
        }
        if (policy == SBMP_ROUTE_POLICY_LOC_RIB)
        {
            return client->rib_v6u_loc_rib;
        }
        return client->rib_v6u_pre;
    }

    return NULL;
}

static void sbmp_client_remove_peer_routes(sbmp_client_t *client, const char *peer_ip)
{
    if (!client || !peer_ip || peer_ip[0] == '\0')
    {
        return;
    }

    net_addr_t peer_addr;
    if (net_addr_from_str(peer_ip, &peer_addr) != 0)
    {
        return;
    }

    sbmp_rib_remove_source(client->rib_v4u_pre, &peer_addr, NULL, NULL);
    sbmp_rib_remove_source(client->rib_v4u_post, &peer_addr, NULL, NULL);
    sbmp_rib_remove_source(client->rib_v4u_loc_rib, &peer_addr, NULL, NULL);
    sbmp_rib_remove_source(client->rib_v6u_pre, &peer_addr, NULL, NULL);
    sbmp_rib_remove_source(client->rib_v6u_post, &peer_addr, NULL, NULL);
    sbmp_rib_remove_source(client->rib_v6u_loc_rib, &peer_addr, NULL, NULL);
}

// ============================================================================
// 运行态锁与查询 API（供 CLI 使用）
// ============================================================================

void sbmp_runtime_lock(void)
{
    if (!g_sbmp_local)
    {
        return;
    }
    pthread_mutex_lock(&g_sbmp_local->runtime_mutex);
}

void sbmp_runtime_unlock(void)
{
    if (!g_sbmp_local)
    {
        return;
    }
    pthread_mutex_unlock(&g_sbmp_local->runtime_mutex);
}

sbmp_client_t *sbmp_runtime_find_client_locked(const char *client_id)
{
    if (!g_sbmp_local || !g_sbmp_local->client_hash || !client_id || client_id[0] == '\0')
    {
        return NULL;
    }

    return (sbmp_client_t *)g_hash_table_lookup(g_sbmp_local->client_hash, client_id);
}

// ============================================================================
// BMP 解析辅助
// ============================================================================

static void sbmp_apply_update_to_rib(sbmp_client_t *client, const char *peer_ip, const bgp_update_result_t *upd,
                                     sbmp_route_policy_t policy)
{
    if (!client || !peer_ip || !upd)
    {
        return;
    }

    uint16_t afi = upd->afi;
    uint8_t safi = upd->safi;
    if (afi == 0 && safi == 0)
    {
        /* legacy UPDATE 默认归入 IPv4 unicast */
        afi = BGP_AFI_IPV4;
        safi = BGP_SAFI_UNICAST;
    }

    sbmp_rib_t *rib = sbmp_client_get_rib(client, afi, safi, policy);

    if (!rib)
    {
        return;
    }

    net_addr_t peer_addr;
    if (net_addr_from_str(peer_ip, &peer_addr) != 0)
    {
        return;
    }

    sbmp_rib_update_stats_t stats;
    sbmp_rib_apply_update(rib, &peer_addr, upd, &stats);
}

static void sbmp_handle_route_monitoring(sbmp_client_t *client, const uint8_t *msg, uint32_t msg_len)
{
    sbmp_bmp_peer_view_t ph;
    const uint8_t *bgp_pdu = NULL;
    uint32_t bgp_pdu_len = 0;
    sbmp_route_policy_t policy = SBMP_ROUTE_POLICY_PRE;

    if (!client || !msg)
    {
        if (client)
        {
            client->msg_parse_err++;
        }
        return;
    }

    if (sbmp_bmp_extract_route_monitoring(msg, msg_len, &ph, &bgp_pdu, &bgp_pdu_len) != 0)
    {
        client->msg_parse_err++;
        return;
    }
    if (ph.is_loc_rib)
    {
        policy = SBMP_ROUTE_POLICY_LOC_RIB;
    }
    else
    {
        policy = ph.is_post_policy ? SBMP_ROUTE_POLICY_POST : SBMP_ROUTE_POLICY_PRE;
    }

    sbmp_peer_t *peer = sbmp_client_get_or_create_peer(client, ph.peer_ip);
    peer->peer_as = ph.peer_as;
    snprintf(peer->bgp_id, sizeof(peer->bgp_id), "%s", ph.bgp_id);
    peer->is_loc_rib_filtered = ph.is_loc_rib_filtered ? TRUE : FALSE;
    peer->route_monitor_msgs++;
    if (policy == SBMP_ROUTE_POLICY_LOC_RIB)
    {
        client->route_monitor_loc_rib_msgs++;
        peer->route_monitor_loc_rib_msgs++;
    }
    else if (policy == SBMP_ROUTE_POLICY_POST)
    {
        client->route_monitor_post_msgs++;
        peer->route_monitor_post_msgs++;
    }
    else
    {
        client->route_monitor_pre_msgs++;
        peer->route_monitor_pre_msgs++;
    }

    if (bgp_pdu_len < BGP_HEADER_LEN)
    {
        client->msg_parse_err++;
        peer->update_parse_errs++;
        return;
    }

    bgp_pdu_info_t pdu;
    if (bgp_pdu_parse_hdr(bgp_pdu, bgp_pdu_len, &pdu) != 0)
    {
        client->msg_parse_err++;
        peer->update_parse_errs++;
        return;
    }

    switch (pdu.msg_type)
    {
        case BGP_MSG_TYPE_UPDATE:
        {
            bgp_update_result_t *upd = NULL;
            if (bgp_update_parse(pdu.body, pdu.body_len, BGP_PARSE_FLAG_AS4, &upd) != 0 || !upd)
            {
                client->msg_parse_err++;
                peer->update_parse_errs++;
                return;
            }

            sbmp_apply_update_to_rib(client, ph.peer_ip, upd, policy);
            peer->update_msgs++;
            if (policy == SBMP_ROUTE_POLICY_LOC_RIB)
            {
                client->update_loc_rib_msgs++;
                peer->update_loc_rib_msgs++;
            }
            else if (policy == SBMP_ROUTE_POLICY_POST)
            {
                client->update_post_msgs++;
                peer->update_post_msgs++;
            }
            else
            {
                client->update_pre_msgs++;
                peer->update_pre_msgs++;
            }
            bgp_update_result_free(upd);
            break;
        }
        case BGP_MSG_TYPE_OPEN:
        {
            bgp_open_msg_t om;
            if (bgp_open_parse(pdu.body, pdu.body_len, &om) == 0)
            {
                if (om.cap_as4)
                {
                    peer->peer_as = om.cap_as4;
                }
                else
                {
                    peer->peer_as = om.my_as;
                }
                snprintf(peer->bgp_id, sizeof(peer->bgp_id), "%s", om.bgp_id);
            }
            break;
        }
        case BGP_MSG_TYPE_NOTIFICATION:
        {
            bgp_notif_msg_t nm;
            (void)bgp_notif_parse(pdu.body, pdu.body_len, &nm);
            break;
        }
        default:
            break;
    }
}

static void sbmp_handle_peer_state(sbmp_client_t *client, const uint8_t *msg, uint32_t msg_len, gboolean is_up)
{
    if (!client || !msg || msg_len < SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN)
    {
        if (client)
        {
            client->msg_parse_err++;
        }
        return;
    }

    sbmp_bmp_peer_view_t ph;
    if (sbmp_bmp_parse_peer_header(msg + SBMP_BMP_COMMON_HDR_LEN, SBMP_BMP_PEER_HDR_LEN, &ph) != 0)
    {
        client->msg_parse_err++;
        return;
    }

    sbmp_peer_t *peer = sbmp_client_get_or_create_peer(client, ph.peer_ip);
    peer->peer_as = ph.peer_as;
    snprintf(peer->bgp_id, sizeof(peer->bgp_id), "%s", ph.bgp_id);
    peer->is_loc_rib_filtered = ph.is_loc_rib_filtered ? TRUE : FALSE;
    peer->is_up = is_up;
    peer->last_change_usec = g_get_real_time();

    if (is_up)
    {
        peer->up_count++;
    }
    else
    {
        peer->down_count++;
        sbmp_client_remove_peer_routes(client, peer->peer_ip);
    }
}

static void sbmp_handle_stats_report(sbmp_client_t *client, const uint8_t *msg, uint32_t msg_len)
{
    if (!client || !msg || msg_len < SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN)
    {
        if (client)
        {
            client->msg_parse_err++;
        }
        return;
    }

    sbmp_bmp_peer_view_t ph;
    if (sbmp_bmp_parse_peer_header(msg + SBMP_BMP_COMMON_HDR_LEN, SBMP_BMP_PEER_HDR_LEN, &ph) == 0)
    {
        sbmp_peer_t *peer = sbmp_client_get_or_create_peer(client, ph.peer_ip);
        peer->peer_as = ph.peer_as;
        snprintf(peer->bgp_id, sizeof(peer->bgp_id), "%s", ph.bgp_id);
        peer->is_loc_rib_filtered = ph.is_loc_rib_filtered ? TRUE : FALSE;
    }
}

static void sbmp_handle_bmp_message(sbmp_client_t *client, const uint8_t *msg, uint32_t msg_len)
{
    client->msg_total++;
    client->last_active_usec = g_get_real_time();

    uint8_t msg_type = msg[5];
    switch (msg_type)
    {
        case SBMP_BMP_MSG_ROUTE_MONITORING:
            client->route_monitor_msgs++;
            sbmp_handle_route_monitoring(client, msg, msg_len);
            break;
        case SBMP_BMP_MSG_STATS_REPORT:
            client->stats_report_msgs++;
            sbmp_handle_stats_report(client, msg, msg_len);
            break;
        case SBMP_BMP_MSG_PEER_DOWN:
            client->peer_down_msgs++;
            sbmp_handle_peer_state(client, msg, msg_len, FALSE);
            break;
        case SBMP_BMP_MSG_PEER_UP:
            client->peer_up_msgs++;
            sbmp_handle_peer_state(client, msg, msg_len, TRUE);
            break;
        case SBMP_BMP_MSG_INITIATION:
            client->initiation_msgs++;
            break;
        case SBMP_BMP_MSG_TERMINATION:
            client->termination_msgs++;
            break;
        case SBMP_BMP_MSG_ROUTE_MIRRORING:
            client->route_mirroring_msgs++;
            break;
        default:
            client->msg_parse_err++;
            break;
    }
}

static int sbmp_parse_client_stream(sbmp_client_t *client)
{
    while (client->recv_len >= SBMP_BMP_COMMON_HDR_LEN)
    {
        uint8_t version = client->recv_buf[0];
        uint32_t len_be = 0;
        memcpy(&len_be, client->recv_buf + 1, sizeof(len_be));
        uint32_t msg_len = ntohl(len_be);

        if (version != 3 || msg_len < SBMP_BMP_COMMON_HDR_LEN || msg_len > SBMP_BMP_MSG_LEN_MAX)
        {
            client->msg_parse_err++;
            return -1;
        }

        if (client->recv_len < msg_len)
        {
            return 0;
        }

        sbmp_handle_bmp_message(client, client->recv_buf, msg_len);

        uint32_t remain = client->recv_len - msg_len;
        if (remain > 0)
        {
            memmove(client->recv_buf, client->recv_buf + msg_len, remain);
        }
        client->recv_len = remain;
    }

    return 0;
}

// ============================================================================
// BMP server 线程与连接处理
// ============================================================================

static void sbmp_close_client_fd(int fd)
{
    if (fd < 0 || !g_sbmp_local)
    {
        return;
    }

    epoll_ctl(g_sbmp_local->epoll_fd, EPOLL_CTL_DEL, fd, NULL);

    sbmp_runtime_lock();
    sbmp_client_t *client = g_hash_table_lookup(g_sbmp_local->fd_hash, GINT_TO_POINTER(fd));
    if (client)
    {
        g_hash_table_remove(g_sbmp_local->fd_hash, GINT_TO_POINTER(fd));
        if (client->fd == fd)
        {
            client->fd = -1;
            client->connected = FALSE;
            client->source_port = 0;
            client->recv_len = 0;
        }
    }
    sbmp_runtime_unlock();

    close(fd);
}

static void sbmp_handle_client_data(int fd)
{
    gboolean should_close = FALSE;

    sbmp_runtime_lock();
    sbmp_client_t *client = g_hash_table_lookup(g_sbmp_local->fd_hash, GINT_TO_POINTER(fd));
    if (!client)
    {
        sbmp_runtime_unlock();
        sbmp_close_client_fd(fd);
        return;
    }

    while (!should_close)
    {
        if (client->recv_len >= sizeof(client->recv_buf))
        {
            should_close = TRUE;
            break;
        }

        ssize_t n = recv(fd, client->recv_buf + client->recv_len, sizeof(client->recv_buf) - client->recv_len, 0);
        if (n == 0)
        {
            should_close = TRUE;
            break;
        }
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            should_close = TRUE;
            break;
        }

        client->recv_len += (uint32_t)n;

        if (sbmp_parse_client_stream(client) != 0)
        {
            should_close = TRUE;
            break;
        }
    }
    sbmp_runtime_unlock();

    if (should_close)
    {
        sbmp_close_client_fd(fd);
    }
}

static void sbmp_accept_clients(void)
{
    while (1)
    {
        struct sockaddr_in sa;
        socklen_t slen = sizeof(sa);
        int fd = accept(g_sbmp_local->listen_fd, (struct sockaddr *)&sa, &slen);
        if (fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            LOG_PERROR("SBMP: accept failed");
            break;
        }

        if (set_nonblock(fd) != 0)
        {
            LOG_PERROR("SBMP: Failed to set client fd to non-blocking");
            close(fd);
            continue;
        }

        char ip[SBMP_CLIENT_ID_MAX];
        if (!inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip)))
        {
            close(fd);
            continue;
        }

        uint16_t port = ntohs(sa.sin_port);

        int old_fd = -1;
        sbmp_runtime_lock();

        sbmp_client_t *client = sbmp_runtime_find_client_locked(ip);
        if (!client)
        {
            client = sbmp_client_create(ip, ip, port, fd);
            g_hash_table_insert(g_sbmp_local->client_hash, g_strdup(ip), client);
        }
        else
        {
            if (client->connected && client->fd >= 0 && client->fd != fd)
            {
                old_fd = client->fd;
                g_hash_table_remove(g_sbmp_local->fd_hash, GINT_TO_POINTER(old_fd));
            }

            snprintf(client->source_ip, sizeof(client->source_ip), "%s", ip);
            client->source_port = port;
            client->fd = fd;
            client->connected = TRUE;
            client->connected_at_usec = g_get_real_time();
            client->last_active_usec = client->connected_at_usec;
            client->recv_len = 0;
        }

        g_hash_table_insert(g_sbmp_local->fd_hash, GINT_TO_POINTER(fd), client);

        sbmp_runtime_unlock();

        if (old_fd >= 0)
        {
            sbmp_close_client_fd(old_fd);
        }

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
        ev.data.fd = fd;
        if (epoll_ctl(g_sbmp_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
        {
            LOG_PERROR("SBMP: epoll_ctl ADD client fd failed");
            sbmp_close_client_fd(fd);
            continue;
        }

        LOG_INFO("SBMP: BMP client connected: %s:%u (fd=%d)", ip, port, fd);
    }
}

static void *sbmp_server_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "sbmp-server");
    log_set_tag("sbmp");

    struct epoll_event events[SBMP_MAX_EPOLL_EVENTS];

    while (g_sbmp_local && g_sbmp_local->running)
    {
        int nfds = epoll_wait(g_sbmp_local->epoll_fd, events, SBMP_MAX_EPOLL_EVENTS, 1000);
        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("SBMP: epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;
            if (fd < 0)
            {
                continue;
            }

            if (g_sbmp_local->listen_fd >= 0 && fd == g_sbmp_local->listen_fd)
            {
                sbmp_accept_clients();
                continue;
            }

            if ((events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0)
            {
                sbmp_close_client_fd(fd);
                continue;
            }

            if ((events[i].events & EPOLLIN) != 0)
            {
                sbmp_handle_client_data(fd);
            }
        }
    }

    return NULL;
}

// ============================================================================
// BMP listen socket 管理
// ============================================================================

void sbmp_listen_start(uint16_t port)
{
    if (!g_sbmp_local)
    {
        return;
    }
    if (g_sbmp_local->listen_fd >= 0)
    {
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_PERROR("SBMP: Failed to create listen socket");
        return;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    if (set_nonblock(fd) != 0)
    {
        LOG_PERROR("SBMP: Failed to set listen socket to non-blocking");
        close(fd);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_PERROR("SBMP: bind 0.0.0.0:%u failed", port);
        close(fd);
        return;
    }

    if (listen(fd, 32) < 0)
    {
        LOG_PERROR("SBMP: listen failed");
        close(fd);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(g_sbmp_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("SBMP: epoll_ctl ADD listen fd failed");
        close(fd);
        return;
    }

    g_sbmp_local->listen_fd = fd;
    g_sbmp_local->server_port = port;
    LOG_INFO("SBMP: Listening on 0.0.0.0:%u (fd=%d)", port, fd);
}

void sbmp_listen_stop(void)
{
    if (!g_sbmp_local || g_sbmp_local->listen_fd < 0)
    {
        return;
    }

    epoll_ctl(g_sbmp_local->epoll_fd, EPOLL_CTL_DEL, g_sbmp_local->listen_fd, NULL);
    close(g_sbmp_local->listen_fd);
    g_sbmp_local->listen_fd = -1;
    LOG_INFO("SBMP: Stopped listening on port %u", g_sbmp_local->server_port);
}

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_SBMP,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START - Establishing IPC connections到 CFG
// ============================================================================

static void sbmp_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = sbmp_local_ipc_ctx();
    LOG_INFO("SBMP Phase 1: MODULE_START - Establishing IPC connections");
    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    LOG_INFO("SBMP: Connected to CFG and DB");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留
// ============================================================================

static void sbmp_on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = sbmp_local_ipc_ctx();
    LOG_INFO("SBMP Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 建表 + DB 恢复
// ============================================================================

static void sbmp_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = sbmp_local_ipc_ctx();
    LOG_INFO("SBMP Phase 3: MODULE_READY - Initializing database tables and restoring state");

    if (sbmp_db_init() != 0)
    {
        LOG_ERROR("SBMP: Database table initialization failed");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    if (sbmp_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SBMP: Failed to restore state from database");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void sbmp_on_shutdown(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = sbmp_local_ipc_ctx();
    LOG_INFO("SBMP: Cleaning up module state");

    sbmp_cli_cleanup_state();

    sbmp_listen_stop();

    g_sbmp_local->running = 0;
    if (g_sbmp_local->server_thread)
    {
        pthread_join(g_sbmp_local->server_thread, NULL);
        g_sbmp_local->server_thread = 0;
    }

    if (g_sbmp_local->epoll_fd >= 0)
    {
        close(g_sbmp_local->epoll_fd);
        g_sbmp_local->epoll_fd = -1;
    }

    if (g_sbmp_local->fd_hash)
    {
        g_hash_table_destroy(g_sbmp_local->fd_hash);
        g_sbmp_local->fd_hash = NULL;
    }
    if (g_sbmp_local->client_hash)
    {
        g_hash_table_destroy(g_sbmp_local->client_hash);
        g_sbmp_local->client_hash = NULL;
    }
    pthread_mutex_destroy(&g_sbmp_local->runtime_mutex);

    g_sbmp_local->dev_ipc_ctx = NULL;
    g_free(g_sbmp_local);
    g_sbmp_local = NULL;

    LOG_INFO("SBMP: Module cleanup complete");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void sbmp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            sbmp_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            sbmp_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            sbmp_on_ready(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            sbmp_on_shutdown(msg);
            return;
        case CLI_MSG_TYPE:
            LOG_DEBUG("SBMP: Received CLI command message");
            sbmp_cli_handle_message(msg);
            break;
        case CLI_MSG_TYPE_CONTINUE:
            LOG_DEBUG("SBMP: Received CLI continue request");
            sbmp_cli_handle_continue(msg);
            break;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("SBMP: Received show current-configuration request");
            sbmp_bdr_show_config(msg);
            return;
        default:
            LOG_WARN("SBMP: Unknown message type: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// Module initialization
// ============================================================================

int sbmp_module_init(void)
{
    LOG_INFO("Module initialization");

    /* 复用 bgp_parse 库解析 BMP 携带的 BGP PDU */
    bgp_parse_init();

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_SBMP, "sbmp", DEV_MODULE_PORT_SBMP, sbmp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_sbmp_local = g_malloc0(sizeof(sbmp_local_t));
    g_sbmp_local->dev_ipc_ctx = ctx;
    g_sbmp_local->server_port = 0;
    g_sbmp_local->listen_fd = -1;

    g_sbmp_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_sbmp_local->epoll_fd < 0)
    {
        LOG_PERROR("SBMP: Failed to create epoll");
        g_free(g_sbmp_local);
        g_sbmp_local = NULL;
        return -1;
    }

    g_sbmp_local->client_hash =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)sbmp_client_destroy);
    g_sbmp_local->fd_hash = g_hash_table_new(g_direct_hash, g_direct_equal);
    pthread_mutex_init(&g_sbmp_local->runtime_mutex, NULL);

    g_sbmp_local->running = 1;
    if (pthread_create(&g_sbmp_local->server_thread, NULL, sbmp_server_thread, NULL) != 0)
    {
        LOG_PERROR("SBMP: Failed to create server thread");
        g_hash_table_destroy(g_sbmp_local->fd_hash);
        g_hash_table_destroy(g_sbmp_local->client_hash);
        close(g_sbmp_local->epoll_fd);
        pthread_mutex_destroy(&g_sbmp_local->runtime_mutex);
        g_free(g_sbmp_local);
        g_sbmp_local = NULL;
        return -1;
    }

    return 0;
}
