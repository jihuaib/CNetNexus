/**
 * @file   ldp_session.c
 * @brief  LDP TCP session 状态机实现
 *
 * M3 范围：建立 TCP / 协商 Initialization / 周期 KeepAlive / 状态展示。
 * 暂不实现 Address / Label Mapping 等业务消息（M4 起）。
 *
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errcode.h"
#include "ldp.h"
#include "ldp_lib.h"
#include "ldp_pkt.h"
#include "ldp_route_sync.h"
#include "ldp_worker.h"
#include "log.h"

/* RFC 5036 §2.5.3：active 端在收到对端 hello 后不立即 connect，给对端 hello
 * 也到达本端的窗口；否则 passive 端因为还没建 adjacency 会拒绝连接。*/
#define LDP_INIT_CONNECT_DELAY_MS 1500u

/* pending accept 在 5s 内未匹配到 adjacency 就关闭。*/
#define LDP_PENDING_ACCEPT_TIMEOUT_MS 5000u

static uint64_t now_msec(void);
static int register_peer_fd(ldp_peer_t *p, uint32_t events);

typedef struct ldp_pending_accept
{
    int fd;
    uint32_t src_v4; /**< host order */
    uint64_t accept_msec;
} ldp_pending_accept_t;

static GList *g_pending_accepts = NULL; /* 元素：ldp_pending_accept_t* */

static void pending_close_and_free(ldp_pending_accept_t *pa)
{
    if (!pa)
    {
        return;
    }
    if (pa->fd >= 0)
    {
        if (g_ldp_work_local && g_ldp_work_local->epoll_fd >= 0)
        {
            (void)epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_DEL, pa->fd, NULL);
        }
        close(pa->fd);
        pa->fd = -1;
    }
    g_free(pa);
}

static void pending_attach_to_peer(ldp_peer_t *match, ldp_pending_accept_t *pa)
{
    if (!match || !pa || pa->fd < 0)
    {
        return;
    }
    /* 摘掉 pending 上挂的 epoll 注册，再以 PEER 类型重新注册到 peer fd 上 */
    if (g_ldp_work_local && g_ldp_work_local->epoll_fd >= 0)
    {
        (void)epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_DEL, pa->fd, NULL);
    }
    match->fd = pa->fd;
    match->state = LDP_PEER_INITIALIZED;
    match->connecting_since_msec = now_msec();
    if (register_peer_fd(match, EPOLLIN | EPOLLRDHUP) < 0)
    {
        close(pa->fd);
        match->fd = -1;
        return;
    }
    char ip[16];
    ldp_worker_format_lsr_id(match->peer_lsr_id, ip, sizeof(ip));
    LOG_INFO("LDP: passive accept attached to peer %s (transport %u.%u.%u.%u)", ip, (pa->src_v4 >> 24) & 0xFF,
             (pa->src_v4 >> 16) & 0xFF, (pa->src_v4 >> 8) & 0xFF, pa->src_v4 & 0xFF);
    /* 标记 pa->fd 为 -1，避免后续 free 时又 close */
    pa->fd = -1;
}

void ldp_session_pending_promote_for_transport(uint32_t transport_v4)
{
    GList *next = NULL;
    for (GList *it = g_pending_accepts; it; it = next)
    {
        next = it->next;
        ldp_pending_accept_t *pa = (ldp_pending_accept_t *)it->data;
        if (!pa || pa->src_v4 != transport_v4)
        {
            continue;
        }
        ldp_peer_t *match = NULL;
        if (g_ldp_work_local->adjacencies)
        {
            GHashTableIter ait;
            gpointer ak = NULL, av = NULL;
            g_hash_table_iter_init(&ait, g_ldp_work_local->adjacencies);
            while (g_hash_table_iter_next(&ait, &ak, &av))
            {
                const ldp_adjacency_t *adj = (const ldp_adjacency_t *)av;
                if (!adj || adj->peer_transport_v4 != transport_v4)
                {
                    continue;
                }
                ldp_peer_t *cand = ldp_session_lookup(adj->peer_lsr_id, adj->peer_label_space);
                if (cand && !cand->is_active && cand->fd < 0)
                {
                    match = cand;
                    break;
                }
            }
        }
        if (match)
        {
            pending_attach_to_peer(match, pa);
        }
        g_pending_accepts = g_list_delete_link(g_pending_accepts, it);
        pending_close_and_free(pa);
    }
}

static void pending_expire(uint64_t now_msec_v)
{
    GList *next = NULL;
    for (GList *it = g_pending_accepts; it; it = next)
    {
        next = it->next;
        ldp_pending_accept_t *pa = (ldp_pending_accept_t *)it->data;
        if (!pa)
        {
            g_pending_accepts = g_list_delete_link(g_pending_accepts, it);
            continue;
        }
        if (now_msec_v - pa->accept_msec >= LDP_PENDING_ACCEPT_TIMEOUT_MS)
        {
            char ip[16];
            ldp_worker_format_lsr_id(pa->src_v4, ip, sizeof(ip));
            LOG_INFO("LDP: pending accept from %s expired (no adjacency in %ums)", ip, LDP_PENDING_ACCEPT_TIMEOUT_MS);
            g_pending_accepts = g_list_delete_link(g_pending_accepts, it);
            pending_close_and_free(pa);
        }
    }
}

void ldp_session_pending_handle_io(int fd, uint32_t events)
{
    /* fd 上来事件：要么对端关了（EPOLLHUP/RDHUP/ERR/EOF），要么发了数据。
     * 简化：任何事件都直接关闭并释放 pending —— 对端正常 active 端会先发 Init，
     * 但只有 adjacency 到达后我们才能识别 LSR-ID。所以宁可关掉等下一次连接。*/
    GList *next = NULL;
    for (GList *it = g_pending_accepts; it; it = next)
    {
        next = it->next;
        ldp_pending_accept_t *pa = (ldp_pending_accept_t *)it->data;
        if (!pa || pa->fd != fd)
        {
            continue;
        }
        char ip[16];
        ldp_worker_format_lsr_id(pa->src_v4, ip, sizeof(ip));
        LOG_INFO("LDP: pending accept from %s closed (events=0x%x)", ip, events);
        g_pending_accepts = g_list_delete_link(g_pending_accepts, it);
        pending_close_and_free(pa);
        return;
    }
    /* 找不到对应 pending（可能已被 promote），fd 也已不在 epoll 里，安全跳过 */
}

static uint64_t now_msec(void)
{
    return ldp_worker_now_msec();
}

const char *ldp_session_state_str(ldp_peer_state_t st)
{
    switch (st)
    {
        case LDP_PEER_NON_EXIST:
            return "NON_EXIST";
        case LDP_PEER_INITIALIZED:
            return "INITIALIZED";
        case LDP_PEER_OPEN_SENT:
            return "OPEN_SENT";
        case LDP_PEER_OPEN_REC:
            return "OPEN_REC";
        case LDP_PEER_OPEN_CONFIRM:
            return "OPEN_CONFIRM";
        case LDP_PEER_OPERATIONAL:
            return "OPERATIONAL";
        default:
            return "?";
    }
}

static guint64 peer_key(uint32_t lsr, uint16_t space)
{
    return ((guint64)lsr << 16) | (guint64)space;
}

ldp_peer_t *ldp_session_lookup(uint32_t lsr, uint16_t space)
{
    if (!g_ldp_work_local || !g_ldp_work_local->peers)
    {
        return NULL;
    }
    guint64 k = peer_key(lsr, space);
    return (ldp_peer_t *)g_hash_table_lookup(g_ldp_work_local->peers, &k);
}

static ldp_peer_t *peer_create(uint32_t lsr, uint16_t space, int active)
{
    if (!g_ldp_work_local || !g_ldp_work_local->peers)
    {
        return NULL;
    }
    ldp_peer_t *p = g_malloc0(sizeof(*p));
    if (!p)
    {
        return NULL;
    }
    p->peer_lsr_id = lsr;
    p->peer_label_space = space;
    p->is_active = active ? 1u : 0u;
    p->fd = -1;
    p->state = LDP_PEER_NON_EXIST;
    p->our_keepalive_ms =
        g_ldp_work_local->proto.keepalive_ms ? g_ldp_work_local->proto.keepalive_ms : LDP_DEFAULT_KEEPALIVE_INTERVAL_MS;
    guint64 *kheap = g_malloc(sizeof(*kheap));
    *kheap = peer_key(lsr, space);
    g_hash_table_insert(g_ldp_work_local->peers, kheap, p);
    return p;
}

static void peer_close_socket(ldp_peer_t *p)
{
    if (!p || p->fd < 0)
    {
        return;
    }
    if (g_ldp_work_local && g_ldp_work_local->epoll_fd >= 0)
    {
        (void)epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_DEL, p->fd, NULL);
    }
    close(p->fd);
    p->fd = -1;
    p->state = LDP_PEER_NON_EXIST;
    p->rx_len = 0;
    p->tx_len = 0;
}

void ldp_session_close(ldp_peer_t *peer, const char *reason)
{
    if (!peer)
    {
        return;
    }
    char ip[16];
    ldp_worker_format_lsr_id(peer->peer_lsr_id, ip, sizeof(ip));
    LOG_INFO("LDP: session %s:%u closed: %s", ip, peer->peer_label_space, reason ? reason : "");
    ldp_route_sync_on_session_down(peer->peer_lsr_id, peer->peer_label_space);
    ldp_lib_purge_peer(peer->peer_lsr_id, peer->peer_label_space);
    peer_close_socket(peer);
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int register_peer_fd(ldp_peer_t *p, uint32_t events)
{
    if (!p || p->fd < 0)
    {
        return -1;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.u64 = LDP_EVT_PACK(LDP_EVT_KIND_PEER, p->fd);
    if (epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_ADD, p->fd, &ev) < 0)
    {
        LOG_PERROR("LDP: epoll_ctl ADD peer fd");
        return -1;
    }
    return 0;
}

static int modify_peer_fd(ldp_peer_t *p, uint32_t events)
{
    if (!p || p->fd < 0)
    {
        return -1;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.u64 = LDP_EVT_PACK(LDP_EVT_KIND_PEER, p->fd);
    return epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_MOD, p->fd, &ev);
}

static int peer_send_buf(ldp_peer_t *p, const uint8_t *buf, size_t n)
{
    if (!p || p->fd < 0 || n == 0)
    {
        return -1;
    }
    if (p->tx_len + n > LDP_SESSION_TX_BUF_CAP)
    {
        ldp_session_close(p, "tx buffer overflow");
        return -1;
    }
    memcpy(p->tx_buf + p->tx_len, buf, n);
    p->tx_len += n;

    /* 立即尝试 send；剩余字节由 EPOLLOUT 触发再写 */
    while (p->tx_len > 0)
    {
        ssize_t s = send(p->fd, p->tx_buf, p->tx_len, MSG_NOSIGNAL);
        if (s > 0)
        {
            memmove(p->tx_buf, p->tx_buf + s, p->tx_len - (size_t)s);
            p->tx_len -= (size_t)s;
            p->last_tx_msec = now_msec();
            continue;
        }
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            /* 注册 EPOLLOUT 等可写 */
            (void)modify_peer_fd(p, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
            return 0;
        }
        ldp_session_close(p, "send failed");
        return -1;
    }
    (void)modify_peer_fd(p, EPOLLIN | EPOLLRDHUP);
    return 0;
}

static void send_init(ldp_peer_t *p)
{
    if (!p || !g_ldp_work_local)
    {
        return;
    }
    uint8_t buf[64];
    uint32_t self_lsr = g_ldp_work_local->proto.lsr_id;
    uint16_t self_space = 0u;
    uint16_t ka_sec = (uint16_t)((p->our_keepalive_ms + 999u) / 1000u);
    if (ka_sec == 0u)
    {
        ka_sec = (uint16_t)((LDP_DEFAULT_KEEPALIVE_INTERVAL_MS + 999u) / 1000u);
    }
    int n = ldp_pkt_encode_init(self_lsr, self_space, ++p->next_msg_id, ka_sec, p->peer_lsr_id, p->peer_label_space,
                                buf, sizeof(buf));
    if (n <= 0)
    {
        ldp_session_close(p, "encode init failed");
        return;
    }
    (void)peer_send_buf(p, buf, (size_t)n);
}

static void send_address(ldp_peer_t *p)
{
    if (!p || !g_ldp_work_local || !g_ldp_work_local->interfaces)
    {
        return;
    }
    /* 收集所有有 IPv4 地址的使能接口，再加 LSR-ID（loopback 当成本端地址） */
    uint32_t addrs[32];
    size_t n = 0;
    if (g_ldp_work_local->proto.lsr_id != 0u && n < G_N_ELEMENTS(addrs))
    {
        addrs[n++] = g_ldp_work_local->proto.lsr_id;
    }
    GHashTableIter it;
    gpointer k = NULL, v = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->interfaces);
    while (g_hash_table_iter_next(&it, &k, &v) && n < G_N_ELEMENTS(addrs))
    {
        ldp_iface_state_t *iface = (ldp_iface_state_t *)v;
        if (iface && iface->enabled && iface->ipv4_local != 0u)
        {
            int dup = 0;
            for (size_t i = 0; i < n; i++)
            {
                if (addrs[i] == iface->ipv4_local)
                {
                    dup = 1;
                    break;
                }
            }
            if (!dup)
            {
                addrs[n++] = iface->ipv4_local;
            }
        }
    }

    uint8_t buf[256];
    int sz = ldp_pkt_encode_address(g_ldp_work_local->proto.lsr_id, 0u, ++p->next_msg_id, 0 /*Address*/, addrs, n, buf,
                                    sizeof(buf));
    if (sz <= 0)
    {
        return;
    }
    (void)peer_send_buf(p, buf, (size_t)sz);
}

static void send_label_mapping(ldp_peer_t *p, uint32_t prefix, uint8_t prefix_len, uint32_t label)
{
    if (!p || !g_ldp_work_local)
    {
        return;
    }
    uint8_t buf[64];
    int sz = ldp_pkt_encode_label_mapping(g_ldp_work_local->proto.lsr_id, 0u, ++p->next_msg_id, prefix, prefix_len,
                                          label, buf, sizeof(buf));
    if (sz <= 0)
    {
        return;
    }
    (void)peer_send_buf(p, buf, (size_t)sz);
}

static void advertise_local_fecs(ldp_peer_t *p)
{
    if (!p || !g_ldp_work_local)
    {
        return;
    }
    /* M4：仅自动通告 LSR-ID/32 一条 host route，使用 implicit-null（PHP）。 */
    if (g_ldp_work_local->proto.lsr_id == 0u)
    {
        return;
    }
    ldp_fec_t fec;
    fec.prefix = g_ldp_work_local->proto.lsr_id;
    fec.prefix_len = 32u;
    /* 入口（egress）LSR 通告 implicit-null 让上游执行 PHP */
    (void)ldp_lib_alloc_local_label(&fec); /* 占位，方便 show 显示 */
    send_label_mapping(p, fec.prefix, fec.prefix_len, LDP_LABEL_IMPLICIT_NULL);
}

static void on_session_operational(ldp_peer_t *p)
{
    if (!p)
    {
        return;
    }
    char ip[16];
    ldp_worker_format_lsr_id(p->peer_lsr_id, ip, sizeof(ip));
    LOG_INFO("LDP: session OPERATIONAL with %s:%u", ip, p->peer_label_space);
    send_address(p);
    advertise_local_fecs(p);
    ldp_route_sync_on_session_up(p->peer_lsr_id, p->peer_label_space);
}

static void send_keepalive(ldp_peer_t *p)
{
    if (!p)
    {
        return;
    }
    uint8_t buf[32];
    int n = ldp_pkt_encode_keepalive(g_ldp_work_local->proto.lsr_id, 0u, ++p->next_msg_id, buf, sizeof(buf));
    if (n <= 0)
    {
        ldp_session_close(p, "encode keepalive failed");
        return;
    }
    (void)peer_send_buf(p, buf, (size_t)n);
}

static void send_notification(ldp_peer_t *p, uint32_t status_code, int fatal, uint32_t ref_msg_id,
                              uint16_t ref_msg_type)
{
    if (!p || !g_ldp_work_local || p->fd < 0)
    {
        return;
    }
    uint8_t buf[64];
    int n = ldp_pkt_encode_notification(g_ldp_work_local->proto.lsr_id, 0u, ++p->next_msg_id, status_code, fatal,
                                        ref_msg_id, ref_msg_type, buf, sizeof(buf));
    if (n <= 0)
    {
        return;
    }
    ssize_t s = send(p->fd, buf, (size_t)n, MSG_NOSIGNAL);
    if (s > 0)
    {
        p->last_tx_msec = now_msec();
    }
}

static void send_label_release(ldp_peer_t *p, uint32_t prefix, uint8_t prefix_len)
{
    if (!p || !g_ldp_work_local)
    {
        return;
    }
    uint8_t buf[64];
    int n = ldp_pkt_encode_label_release(g_ldp_work_local->proto.lsr_id, 0u, ++p->next_msg_id, prefix, prefix_len, 0u,
                                         0, buf, sizeof(buf));
    if (n <= 0)
    {
        return;
    }
    (void)peer_send_buf(p, buf, (size_t)n);
}

static int connect_active(ldp_peer_t *p, uint32_t peer_transport_v4)
{
    if (!p)
    {
        return -1;
    }
    if (peer_transport_v4 == 0u)
    {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
    {
        LOG_PERROR("LDP: socket(TCP)");
        return -1;
    }
    if (set_nonblock(fd) < 0)
    {
        close(fd);
        return -1;
    }
    int yes = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(LDP_PORT);
    dst.sin_addr.s_addr = htonl(peer_transport_v4);

    int rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
    if (rc < 0 && errno != EINPROGRESS)
    {
        char ip[16];
        ldp_worker_format_lsr_id(peer_transport_v4, ip, sizeof(ip));
        LOG_WARN("LDP: connect %s:%u failed: %s", ip, LDP_PORT, strerror(errno));
        close(fd);
        return -1;
    }

    p->fd = fd;
    p->state = LDP_PEER_INITIALIZED;
    p->connecting_since_msec = now_msec();
    if (register_peer_fd(p, EPOLLIN | EPOLLOUT | EPOLLRDHUP) < 0)
    {
        close(fd);
        p->fd = -1;
        return -1;
    }
    char ip[16];
    ldp_worker_format_lsr_id(peer_transport_v4, ip, sizeof(ip));
    LOG_INFO("LDP: active connect %s:%u (peer %u)", ip, LDP_PORT, p->peer_lsr_id);
    return 0;
}

void ldp_session_on_adjacency_up(uint32_t peer_lsr, uint16_t peer_space, uint32_t peer_xport, uint32_t self_xport,
                                 uint32_t peer_link_addr)
{
    if (!g_ldp_work_local || g_ldp_work_local->proto.lsr_id == 0u)
    {
        return;
    }
    uint32_t local_xport = self_xport ? self_xport : g_ldp_work_local->proto.lsr_id;
    uint32_t remote_xport = peer_xport ? peer_xport : peer_link_addr;
    if (remote_xport == 0u)
    {
        remote_xport = peer_lsr;
    }
    int active = (local_xport > remote_xport) ? 1 : 0;
    ldp_peer_t *p = ldp_session_lookup(peer_lsr, peer_space);
    if (!p)
    {
        p = peer_create(peer_lsr, peer_space, active);
        if (!p)
        {
            return;
        }
    }
    p->is_active = active ? 1u : 0u;
    p->peer_transport_v4 = remote_xport;
    p->self_transport_v4 = local_xport;
    p->peer_link_addr_v4 = peer_link_addr;
    if (p->adj_first_seen_msec == 0u)
    {
        p->adj_first_seen_msec = now_msec();
    }
    /* 邻接到来时刷新 pending accept：对端可能已经先发了 SYN 但 R1 当时没认出来；
     * 现在 transport 已知，可以把 pending fd 接管到 peer。*/
    ldp_session_pending_promote_for_transport(p->peer_transport_v4);
}

void ldp_session_on_adjacency_down(uint32_t peer_lsr, uint16_t peer_space)
{
    ldp_peer_t *p = ldp_session_lookup(peer_lsr, peer_space);
    if (!p)
    {
        return;
    }
    ldp_session_close(p, "adjacency down");
    g_hash_table_remove(g_ldp_work_local->peers, &(guint64){peer_key(peer_lsr, peer_space)});
}

int ldp_session_open_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
    {
        LOG_PERROR("LDP: TCP listen socket");
        return -1;
    }
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (set_nonblock(fd) < 0)
    {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(LDP_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_WARN("LDP: bind TCP/%u failed: %s", LDP_PORT, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0)
    {
        LOG_PERROR("LDP: listen");
        close(fd);
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = LDP_EVT_PACK(LDP_EVT_KIND_LISTEN, fd);
    if (epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("LDP: epoll_ctl ADD listen");
        close(fd);
        return -1;
    }

    g_ldp_work_local->tcp_listen_fd = fd;
    LOG_INFO("LDP: listening on TCP/%u", LDP_PORT);
    return 0;
}

void ldp_session_close_listener(void)
{
    if (!g_ldp_work_local || g_ldp_work_local->tcp_listen_fd < 0)
    {
        return;
    }
    if (g_ldp_work_local->epoll_fd >= 0)
    {
        (void)epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_DEL, g_ldp_work_local->tcp_listen_fd, NULL);
    }
    close(g_ldp_work_local->tcp_listen_fd);
    g_ldp_work_local->tcp_listen_fd = -1;
}

void ldp_session_handle_listen_accept(void)
{
    if (!g_ldp_work_local || g_ldp_work_local->tcp_listen_fd < 0)
    {
        return;
    }
    for (;;)
    {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int fd = accept(g_ldp_work_local->tcp_listen_fd, (struct sockaddr *)&src, &slen);
        if (fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("LDP: accept");
            return;
        }
        (void)set_nonblock(fd);
        int yes = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        /* passive 端匹配：TCP 源 IP 是对端的 transport 地址，按
         * adjacency.peer_transport_v4 反查 (peer_lsr_id, label_space)，
         * 再到 peers 表里取 peer。LSR-ID 通常是 loopback，与 transport 不同，
         * 直接拿 src_v4 当 LSR-ID 是错的。*/
        uint32_t src_v4 = ntohl(src.sin_addr.s_addr);
        ldp_peer_t *match = NULL;
        if (g_ldp_work_local->adjacencies)
        {
            GHashTableIter ait;
            gpointer akey = NULL, aval = NULL;
            g_hash_table_iter_init(&ait, g_ldp_work_local->adjacencies);
            while (g_hash_table_iter_next(&ait, &akey, &aval))
            {
                const ldp_adjacency_t *adj = (const ldp_adjacency_t *)aval;
                if (!adj || adj->peer_transport_v4 != src_v4)
                {
                    continue;
                }
                ldp_peer_t *cand = ldp_session_lookup(adj->peer_lsr_id, adj->peer_label_space);
                if (cand && !cand->is_active && cand->fd < 0)
                {
                    match = cand;
                    break;
                }
            }
        }

        if (!match)
        {
            /* RFC 5036 §2.5.3：对端 active 端在我们还没收到它的 hello 之前
             * 可能就先建 TCP。把 fd 暂存到 pending 队列，等 adjacency 到达
             * （ldp_session_on_adjacency_up 会调 _pending_promote_for_transport）
             * 或者超时（LDP_PENDING_ACCEPT_TIMEOUT_MS）后处理。*/
            ldp_pending_accept_t *pa = g_malloc0(sizeof(*pa));
            pa->fd = fd;
            pa->src_v4 = src_v4;
            pa->accept_msec = now_msec();
            struct epoll_event pev;
            memset(&pev, 0, sizeof(pev));
            pev.events = EPOLLIN | EPOLLRDHUP;
            pev.data.u64 = LDP_EVT_PACK(LDP_EVT_KIND_PENDING_ACCEPT, fd);
            if (epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_ADD, fd, &pev) < 0)
            {
                LOG_PERROR("LDP: epoll_ctl ADD pending accept");
                close(fd);
                g_free(pa);
                continue;
            }
            g_pending_accepts = g_list_prepend(g_pending_accepts, pa);
            char ip[16];
            ldp_worker_format_lsr_id(src_v4, ip, sizeof(ip));
            LOG_INFO("LDP: pending accept from %s (waiting for adjacency)", ip);
            continue;
        }

        match->fd = fd;
        match->state = LDP_PEER_INITIALIZED;
        match->connecting_since_msec = now_msec();
        if (register_peer_fd(match, EPOLLIN | EPOLLRDHUP) < 0)
        {
            close(fd);
            match->fd = -1;
            continue;
        }
        char ip[16];
        ldp_worker_format_lsr_id(match->peer_lsr_id, ip, sizeof(ip));
        LOG_INFO("LDP: passive accept from %s (transport %u.%u.%u.%u)", ip, (src_v4 >> 24) & 0xFF,
                 (src_v4 >> 16) & 0xFF, (src_v4 >> 8) & 0xFF, src_v4 & 0xFF);
    }
}

/* 处理已收到的 PDU 中的所有消息，返回消费字节数；遇致命错误返回 -1 */
static int peer_process_pdu(ldp_peer_t *p, const uint8_t *pdu_buf, size_t pdu_buf_len)
{
    ldp_pdu_hdr_t pdu;
    if (ldp_pkt_parse_pdu_hdr(pdu_buf, pdu_buf_len, &pdu) != 0)
    {
        send_notification(p, LDP_STATUS_BAD_PDU_LENGTH, 1, 0u, 0u);
        return -1;
    }
    /* 主动方：第一帧到达时校验 LSR-ID 与预期匹配 */
    if (p->peer_lsr_id != pdu.lsr_id || p->peer_label_space != pdu.label_space)
    {
        send_notification(p, LDP_STATUS_BAD_LDP_IDENTIFIER, 1, 0u, 0u);
        return -1;
    }

    size_t pos = LDP_PDU_HEADER_SIZE;
    size_t end = (size_t)pdu.pdu_length + 4u;
    if (end > pdu_buf_len)
    {
        end = pdu_buf_len;
    }

    while (pos + LDP_MSG_HEADER_SIZE <= end)
    {
        ldp_msg_hdr_t mh;
        if (ldp_pkt_parse_msg_hdr(pdu_buf + pos, end - pos, &mh) != 0)
        {
            send_notification(p, LDP_STATUS_BAD_MESSAGE_LENGTH, 1, 0u, 0u);
            return -1;
        }
        const uint8_t *body = pdu_buf + pos + LDP_MSG_HEADER_SIZE;
        size_t body_len = (size_t)mh.msg_length > 4u ? (size_t)mh.msg_length - 4u : 0u;

        switch (mh.msg_type)
        {
            case LDP_MSG_TYPE_INITIALIZATION:
            {
                ldp_init_info_t info;
                if (ldp_pkt_parse_init(body, body_len, &info) != 0)
                {
                    send_notification(p, LDP_STATUS_MISSING_MESSAGE_PARAMETERS, 0, mh.msg_id, mh.msg_type);
                    return -1;
                }
                if (info.protocol_version != LDP_VERSION)
                {
                    send_notification(p, LDP_STATUS_BAD_PROTOCOL_VERSION, 1, mh.msg_id, mh.msg_type);
                    return -1;
                }
                if (info.recv_lsr_id != g_ldp_work_local->proto.lsr_id || info.recv_label_space != 0u)
                {
                    send_notification(p, LDP_STATUS_BAD_LDP_IDENTIFIER, 1, mh.msg_id, mh.msg_type);
                    return -1;
                }
                if (info.keepalive_time_sec == 0u)
                {
                    send_notification(p, LDP_STATUS_SESSION_REJECTED_BAD_KEEPALIVE_TIME, 1, mh.msg_id, mh.msg_type);
                    return -1;
                }
                p->peer_keepalive_ms = (uint32_t)info.keepalive_time_sec * 1000u;
                uint32_t my_ka = p->our_keepalive_ms ? p->our_keepalive_ms : LDP_DEFAULT_KEEPALIVE_INTERVAL_MS;
                p->neg_keepalive_ms = (my_ka < p->peer_keepalive_ms) ? my_ka : p->peer_keepalive_ms;
                if (p->neg_keepalive_ms == 0u)
                {
                    p->neg_keepalive_ms = my_ka;
                }
                if (p->is_active)
                {
                    /* 主动方在 OPEN_SENT 收到 Init → 发 KA，进入 OPEN_CONFIRM */
                    p->state = LDP_PEER_OPEN_CONFIRM;
                    send_keepalive(p);
                }
                else
                {
                    /* 被动方：收到 Init → 发 Init+KA，进入 OPEN_REC */
                    send_init(p);
                    send_keepalive(p);
                    p->state = LDP_PEER_OPEN_REC;
                }
                break;
            }
            case LDP_MSG_TYPE_KEEPALIVE:
                if (p->state == LDP_PEER_OPEN_CONFIRM || p->state == LDP_PEER_OPEN_REC)
                {
                    p->state = LDP_PEER_OPERATIONAL;
                    on_session_operational(p);
                }
                break;
            case LDP_MSG_TYPE_NOTIFICATION:
                ldp_session_close(p, "peer notification");
                return (int)end;
            case LDP_MSG_TYPE_ADDRESS:
            case LDP_MSG_TYPE_ADDRESS_WITHDRAW:
            {
                if (p->state != LDP_PEER_OPERATIONAL)
                {
                    break;
                }
                uint32_t addrs[32];
                int n = ldp_pkt_parse_address_list_tlv(body, body_len, addrs, G_N_ELEMENTS(addrs));
                if (n > 0)
                {
                    char ip[16];
                    ldp_worker_format_lsr_id(p->peer_lsr_id, ip, sizeof(ip));
                    LOG_INFO("LDP: peer %s announced %d address(es) (msg=%s)", ip, n,
                             mh.msg_type == LDP_MSG_TYPE_ADDRESS ? "Address" : "AddressWithdraw");
                }
                break;
            }
            case LDP_MSG_TYPE_LABEL_MAPPING:
            {
                if (p->state != LDP_PEER_OPERATIONAL)
                {
                    break;
                }
                uint32_t prefix = 0u;
                uint8_t plen = 0u;
                uint32_t label = 0u;
                if (ldp_pkt_parse_label_msg(body, body_len, &prefix, &plen, &label) == 0)
                {
                    ldp_fec_t fec = {.prefix = prefix, .prefix_len = plen};
                    ldp_lib_set_remote(p->peer_lsr_id, p->peer_label_space, &fec, label);
                    char ip[16], pfx[16];
                    ldp_worker_format_lsr_id(p->peer_lsr_id, ip, sizeof(ip));
                    ldp_worker_format_lsr_id(prefix, pfx, sizeof(pfx));
                    LOG_INFO("LDP: rx LabelMapping %s/%u label=%u from %s", pfx, plen, label, ip);
                    ldp_route_sync_on_remote_label(p->peer_lsr_id, p->peer_label_space, &fec, label);
                }
                break;
            }
            case LDP_MSG_TYPE_LABEL_WITHDRAW:
            {
                if (p->state != LDP_PEER_OPERATIONAL)
                {
                    break;
                }
                uint32_t prefix = 0u;
                uint8_t plen = 0u;
                uint32_t label = 0u;
                if (ldp_pkt_parse_label_msg(body, body_len, &prefix, &plen, &label) == 0)
                {
                    ldp_fec_t fec = {.prefix = prefix, .prefix_len = plen};
                    ldp_route_sync_on_remote_label_withdraw(p->peer_lsr_id, p->peer_label_space, &fec);
                    ldp_lib_del_remote(p->peer_lsr_id, p->peer_label_space, &fec);
                    send_label_release(p, prefix, plen);
                }
                break;
            }
            case LDP_MSG_TYPE_LABEL_RELEASE:
                break;
            default:
                if (!mh.u_bit)
                {
                    send_notification(p, LDP_STATUS_UNKNOWN_MESSAGE_TYPE, 0, mh.msg_id, mh.msg_type);
                }
                break;
        }

        pos += LDP_MSG_HEADER_SIZE + body_len;
    }
    return (int)end;
}

static void peer_drain_rx(ldp_peer_t *p)
{
    if (!p || p->fd < 0)
    {
        return;
    }
    for (;;)
    {
        if (p->rx_len >= LDP_SESSION_RX_BUF_CAP)
        {
            ldp_session_close(p, "rx buffer overflow");
            return;
        }
        ssize_t n = recv(p->fd, p->rx_buf + p->rx_len, LDP_SESSION_RX_BUF_CAP - p->rx_len, 0);
        if (n > 0)
        {
            p->rx_len += (size_t)n;
            p->last_rx_msec = now_msec();
            continue;
        }
        if (n == 0)
        {
            ldp_session_close(p, "peer closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        if (errno == EINTR)
        {
            continue;
        }
        ldp_session_close(p, strerror(errno));
        return;
    }

    /* 解析尽可能多的完整 PDU */
    while (p->rx_len >= LDP_PDU_HEADER_SIZE)
    {
        uint16_t pdu_payload = (uint16_t)((p->rx_buf[2] << 8) | p->rx_buf[3]);
        size_t pdu_full = (size_t)pdu_payload + 4u;
        if (pdu_full > LDP_SESSION_RX_BUF_CAP)
        {
            ldp_session_close(p, "pdu too large");
            return;
        }
        if (p->rx_len < pdu_full)
        {
            return; /* 等下一片 */
        }
        int consumed = peer_process_pdu(p, p->rx_buf, pdu_full);
        if (consumed < 0)
        {
            ldp_session_close(p, "pdu processing failed");
            return;
        }
        if (p->fd < 0)
        {
            return;
        }
        memmove(p->rx_buf, p->rx_buf + consumed, p->rx_len - (size_t)consumed);
        p->rx_len -= (size_t)consumed;
    }
}

static void peer_handle_writable(ldp_peer_t *p)
{
    if (!p || p->fd < 0)
    {
        return;
    }
    /* 主动方：connect 完成检查 */
    if (p->is_active && p->state == LDP_PEER_INITIALIZED)
    {
        int err = 0;
        socklen_t slen = sizeof(err);
        if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &slen) < 0 || err != 0)
        {
            ldp_session_close(p, "connect failed");
            return;
        }
        send_init(p);
        p->state = LDP_PEER_OPEN_SENT;
    }

    /* flush tx buffer */
    while (p->tx_len > 0)
    {
        ssize_t s = send(p->fd, p->tx_buf, p->tx_len, MSG_NOSIGNAL);
        if (s > 0)
        {
            memmove(p->tx_buf, p->tx_buf + s, p->tx_len - (size_t)s);
            p->tx_len -= (size_t)s;
            p->last_tx_msec = now_msec();
            continue;
        }
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return;
        }
        ldp_session_close(p, "send failed");
        return;
    }
    (void)modify_peer_fd(p, EPOLLIN | EPOLLRDHUP);
}

void ldp_session_handle_io(ldp_peer_t *peer, uint32_t events)
{
    if (!peer)
    {
        return;
    }
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
    {
        ldp_session_close(peer, "epoll error/hup");
        return;
    }
    if (events & EPOLLOUT)
    {
        peer_handle_writable(peer);
        if (peer->fd < 0)
        {
            return;
        }
    }
    if (events & EPOLLIN)
    {
        peer_drain_rx(peer);
    }
}

void ldp_session_tick(void)
{
    if (!g_ldp_work_local || !g_ldp_work_local->peers)
    {
        return;
    }
    uint64_t now = now_msec();

    /* pending passive accept 超时清理 */
    pending_expire(now);

    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    GList *to_remove = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->peers);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        ldp_peer_t *p = (ldp_peer_t *)val;
        if (!p)
        {
            continue;
        }
        /* active 端延迟 connect：邻接出现后等 LDP_INIT_CONNECT_DELAY_MS 再发起，
         * 让对端也能在此期间收到我们的 hello、建好 adjacency。*/
        if (p->is_active && p->fd < 0 && p->state == LDP_PEER_NON_EXIST && p->peer_transport_v4 != 0u &&
            p->adj_first_seen_msec != 0u && (now - p->adj_first_seen_msec) >= LDP_INIT_CONNECT_DELAY_MS)
        {
            (void)connect_active(p, p->peer_transport_v4);
        }
        if (p->fd < 0)
        {
            continue;
        }
        if (p->state == LDP_PEER_OPERATIONAL)
        {
            uint32_t ka = p->neg_keepalive_ms ? p->neg_keepalive_ms : p->our_keepalive_ms;
            if (ka == 0u)
            {
                ka = LDP_DEFAULT_KEEPALIVE_INTERVAL_MS;
            }
            if (p->last_tx_msec == 0u || (now - p->last_tx_msec) >= (ka / 3u))
            {
                send_keepalive(p);
            }
            if (p->last_rx_msec != 0u && (now - p->last_rx_msec) > ka)
            {
                LOG_INFO("LDP: session %u keepalive timeout", p->peer_lsr_id);
                send_notification(p, LDP_STATUS_KEEPALIVE_TIMER_EXPIRED, 1, 0u, 0u);
                ldp_session_close(p, "keepalive timeout");
                to_remove = g_list_prepend(to_remove, key);
            }
        }
        else if (p->state == LDP_PEER_INITIALIZED || p->state == LDP_PEER_OPEN_SENT ||
                 p->state == LDP_PEER_OPEN_CONFIRM || p->state == LDP_PEER_OPEN_REC)
        {
            /* 握手 5s 仍未进入 OPERATIONAL 就回退，让 active 端在下次 hello
             * 触发的 adjacency_up 中重连——否则 passive 端的临时拒绝（如对端
             * 还没建立 adjacency）会让握手挂死直到 30s 超时。*/
            if ((now - p->connecting_since_msec) > 5000u)
            {
                LOG_INFO("LDP: session handshake to %u timeout", p->peer_lsr_id);
                ldp_session_close(p, "handshake timeout");
                /* 不从 peers 表移除——保留 peer 槽，下次 adjacency_up 直接复用，
                 * 触发 active 端再 connect。*/
            }
        }
    }
    for (GList *l = to_remove; l; l = l->next)
    {
        g_hash_table_remove(g_ldp_work_local->peers, l->data);
    }
    g_list_free(to_remove);
}

void ldp_session_close_all(void)
{
    if (!g_ldp_work_local || !g_ldp_work_local->peers)
    {
        return;
    }
    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->peers);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        ldp_peer_t *p = (ldp_peer_t *)val;
        if (p)
        {
            peer_close_socket(p);
        }
    }
    g_hash_table_remove_all(g_ldp_work_local->peers);
}
