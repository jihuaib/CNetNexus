/**
 * @file   ldp_discovery.c
 * @brief  LDP basic discovery 实现
 *
 * 每个使能 LDP 的接口绑定一个独立 UDP socket 在 0.0.0.0:646（SO_REUSEADDR），
 * 设置 SO_BINDTODEVICE 限定该接口收发，发送时 dst=224.0.0.2:646；接收时验证
 * 源地址在该接口子网（暂仅校验 ifindex）。
 *
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "errcode.h"
#include "if.h"
#include "ldp_pkt.h"
#include "ldp_session.h"
#include "log.h"

#define LDP_HELLO_BUF_SIZE 256

static uint64_t now_msec(void)
{
    return ldp_worker_now_msec();
}

static void close_iface_socket(ldp_iface_state_t *iface)
{
    if (!iface)
    {
        return;
    }
    if (iface->udp_fd >= 0)
    {
        if (g_ldp_work_local && g_ldp_work_local->epoll_fd >= 0)
        {
            (void)epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_DEL, iface->udp_fd, NULL);
        }
        close(iface->udp_fd);
        iface->udp_fd = -1;
    }
    iface->multicast_joined = 0u;
}

static int join_multicast(int fd, uint32_t ifindex)
{
    struct ip_mreqn mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(LDP_MCAST_GROUP_V4);
    mreq.imr_ifindex = (int)ifindex;
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        LOG_WARN("LDP: IP_ADD_MEMBERSHIP failed on ifindex %u: %s", ifindex, strerror(errno));
        return -1;
    }
    return 0;
}

static int set_multicast_egress(int fd, uint32_t ifindex)
{
    struct ip_mreqn mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_ifindex = (int)ifindex;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq)) < 0)
    {
        LOG_WARN("LDP: IP_MULTICAST_IF failed on ifindex %u: %s", ifindex, strerror(errno));
        return -1;
    }
    uint8_t ttl = 1;
    (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    uint8_t loop = 0;
    (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    int pktinfo = 1;
    (void)setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &pktinfo, sizeof(pktinfo));
    return 0;
}

static int open_iface_socket(ldp_iface_state_t *iface)
{
    if (!iface || iface->ifindex == 0u)
    {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (fd < 0)
    {
        LOG_PERROR("LDP: socket(UDP)");
        return -1;
    }

    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

    /* 限定收发到该接口（需要 CAP_NET_RAW） */
    char devname[IFNAMSIZ] = {0};
    if (if_indextoname(iface->ifindex, devname))
    {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, devname, (socklen_t)strlen(devname)) < 0)
        {
            LOG_WARN("LDP: SO_BINDTODEVICE %s failed: %s", devname, strerror(errno));
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(LDP_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_WARN("LDP: bind UDP/%u on %s failed: %s", LDP_PORT, devname, strerror(errno));
        close(fd);
        return -1;
    }

    if (set_multicast_egress(fd, iface->ifindex) < 0 || join_multicast(fd, iface->ifindex) < 0)
    {
        close(fd);
        return -1;
    }
    iface->multicast_joined = 1u;
    iface->udp_fd = fd;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    /* 不传 iface 指针：用 (KIND_IFACE, fd) 编码，分发时按 fd 反查 iface */
    ev.data.u64 = LDP_EVT_PACK(LDP_EVT_KIND_IFACE, fd);
    if (epoll_ctl(g_ldp_work_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("LDP: epoll_ctl ADD UDP");
        close_iface_socket(iface);
        return -1;
    }

    LOG_INFO("LDP: hello socket up on %s (ifindex=%u)", iface->ifname, iface->ifindex);
    return 0;
}

static void refresh_iface_from_cache(ldp_iface_state_t *iface)
{
    if (!iface)
    {
        return;
    }
    const if_api_cache_entry_t *e = if_api_cache_lookup(iface->ifname);
    if (!e)
    {
        return;
    }
    iface->ifindex = e->ifindex;
    if (e->ipv4_addr.family == AF_INET)
    {
        iface->ipv4_local = ntohl(e->ipv4_addr.u.v4.s_addr);
    }
    else
    {
        iface->ipv4_local = 0u;
    }
    iface->link_up = (e->link_up && e->proto_up) ? 1u : 0u;
}

static void purge_adjacencies_for_iface(const char *ifname)
{
    if (!g_ldp_work_local || !g_ldp_work_local->adjacencies || !ifname)
    {
        return;
    }
    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    GList *to_drop = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->adjacencies);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        ldp_adjacency_t *adj = (ldp_adjacency_t *)val;
        if (adj && strcmp(adj->ifname, ifname) == 0)
        {
            ldp_session_on_adjacency_down(adj->peer_lsr_id, adj->peer_label_space);
            to_drop = g_list_prepend(to_drop, key);
        }
    }
    for (GList *l = to_drop; l; l = l->next)
    {
        g_hash_table_remove(g_ldp_work_local->adjacencies, l->data);
    }
    g_list_free(to_drop);
}

static ldp_iface_state_t *iface_lookup(const char *ifname)
{
    if (!g_ldp_work_local || !g_ldp_work_local->interfaces || !ifname)
    {
        return NULL;
    }
    return (ldp_iface_state_t *)g_hash_table_lookup(g_ldp_work_local->interfaces, ifname);
}

static ldp_iface_state_t *iface_get_or_create(const char *ifname)
{
    ldp_iface_state_t *iface = iface_lookup(ifname);
    if (iface)
    {
        return iface;
    }
    iface = g_malloc0(sizeof(*iface));
    if (!iface)
    {
        return NULL;
    }
    g_strlcpy(iface->ifname, ifname, sizeof(iface->ifname));
    iface->udp_fd = -1;
    g_hash_table_insert(g_ldp_work_local->interfaces, g_strdup(ifname), iface);
    return iface;
}

int ldp_discovery_iface_set(const ldp_if_cfg_t *cfg)
{
    if (!cfg || cfg->ifname[0] == '\0' || !cfg->enabled)
    {
        return ldp_discovery_iface_del(cfg ? cfg->ifname : NULL);
    }

    ldp_iface_state_t *iface = iface_get_or_create(cfg->ifname);
    if (!iface)
    {
        return ERRCODE_FAIL;
    }
    iface->enabled = 1u;
    iface->hello_interval_ms = cfg->hello_interval_ms;
    iface->hold_time_ms = cfg->hold_time_ms;

    refresh_iface_from_cache(iface);

    if (iface->ifindex != 0u && iface->udp_fd < 0 && iface->link_up)
    {
        (void)open_iface_socket(iface);
    }
    return ERRCODE_SUCCESS;
}

int ldp_discovery_iface_del(const char *ifname)
{
    if (!ifname || ifname[0] == '\0' || !g_ldp_work_local || !g_ldp_work_local->interfaces)
    {
        return ERRCODE_FAIL;
    }
    ldp_iface_state_t *iface = iface_lookup(ifname);
    if (!iface)
    {
        return ERRCODE_SUCCESS;
    }
    close_iface_socket(iface);
    purge_adjacencies_for_iface(ifname);
    g_hash_table_remove(g_ldp_work_local->interfaces, ifname);
    return ERRCODE_SUCCESS;
}

void ldp_discovery_proto_changed(uint8_t admin_changed, uint8_t lsr_id_changed)
{
    (void)admin_changed;
    if (!g_ldp_work_local)
    {
        return;
    }
    if (lsr_id_changed && g_ldp_work_local->adjacencies)
    {
        g_hash_table_remove_all(g_ldp_work_local->adjacencies);
    }
}

void ldp_discovery_on_if_event(const char *ifname)
{
    if (!ifname || !g_ldp_work_local)
    {
        return;
    }
    ldp_iface_state_t *iface = iface_lookup(ifname);
    if (!iface)
    {
        return;
    }
    refresh_iface_from_cache(iface);
    if (!iface->link_up || !iface->enabled)
    {
        close_iface_socket(iface);
        purge_adjacencies_for_iface(ifname);
        return;
    }
    if (iface->udp_fd < 0)
    {
        (void)open_iface_socket(iface);
    }
}

static void send_hello_one(ldp_iface_state_t *iface)
{
    if (!iface || iface->udp_fd < 0 || !g_ldp_work_local)
    {
        return;
    }
    if (g_ldp_work_local->proto.lsr_id == 0u || !g_ldp_work_local->proto.admin_up)
    {
        return;
    }

    uint32_t hold_ms = ldp_worker_effective_hold_ms(iface);
    uint16_t hold_sec = (uint16_t)((hold_ms + 999u) / 1000u);
    if (hold_sec == 0u)
    {
        hold_sec = 15u;
    }

    static uint32_t s_msg_id = 1;
    uint8_t buf[LDP_HELLO_BUF_SIZE];
    int n = ldp_pkt_encode_hello(g_ldp_work_local->proto.lsr_id, 0u, s_msg_id++, hold_sec,
                                 iface->ipv4_local /* may be 0 → 不携带 transport TLV */, 0u /* config-seq M2 暂为 0 */,
                                 buf, sizeof(buf));
    if (n <= 0)
    {
        return;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(LDP_PORT);
    dst.sin_addr.s_addr = inet_addr(LDP_MCAST_GROUP_V4);

    ssize_t s = sendto(iface->udp_fd, buf, (size_t)n, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (s < 0)
    {
        if (errno != ENETUNREACH && errno != EAGAIN)
        {
            LOG_WARN("LDP: hello tx on %s failed: %s", iface->ifname, strerror(errno));
        }
        return;
    }
    iface->last_hello_tx_msec = now_msec();
}

static void expire_adjacencies(void)
{
    if (!g_ldp_work_local || !g_ldp_work_local->adjacencies)
    {
        return;
    }
    uint64_t now = now_msec();
    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    GList *to_drop = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->adjacencies);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        ldp_adjacency_t *adj = (ldp_adjacency_t *)val;
        if (!adj)
        {
            continue;
        }
        if (adj->neg_hold_ms == 0u)
        {
            continue; /* 0 视为不过期，按 RFC 用默认 15s */
        }
        if (now > adj->last_seen_msec && (now - adj->last_seen_msec) > adj->neg_hold_ms)
        {
            char buf[16];
            ldp_worker_format_lsr_id(adj->peer_lsr_id, buf, sizeof(buf));
            LOG_INFO("LDP: adjacency %s:%u on %s expired", buf, adj->peer_label_space, adj->ifname);
            ldp_session_on_adjacency_down(adj->peer_lsr_id, adj->peer_label_space);
            to_drop = g_list_prepend(to_drop, key);
        }
    }
    for (GList *l = to_drop; l; l = l->next)
    {
        g_hash_table_remove(g_ldp_work_local->adjacencies, l->data);
    }
    g_list_free(to_drop);
}

void ldp_discovery_tick(void)
{
    if (!g_ldp_work_local || !g_ldp_work_local->interfaces)
    {
        return;
    }
    uint64_t now = now_msec();

    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->interfaces);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        ldp_iface_state_t *iface = (ldp_iface_state_t *)val;
        if (!iface || !iface->enabled || iface->udp_fd < 0 || !iface->link_up)
        {
            continue;
        }
        uint32_t interval = ldp_worker_effective_hello_ms(iface);
        if (iface->last_hello_tx_msec == 0u || (now - iface->last_hello_tx_msec) >= interval)
        {
            send_hello_one(iface);
        }
    }
    expire_adjacencies();
}

void ldp_discovery_handle_rx(ldp_iface_state_t *iface)
{
    if (!iface || iface->udp_fd < 0)
    {
        return;
    }

    for (;;)
    {
        uint8_t buf[1500];
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(iface->udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            if (errno == EINTR)
            {
                continue;
            }
            LOG_WARN("LDP: recvfrom on %s failed: %s", iface->ifname, strerror(errno));
            return;
        }
        if (n < LDP_PDU_HEADER_SIZE)
        {
            continue;
        }

        ldp_pdu_hdr_t pdu;
        if (ldp_pkt_parse_pdu_hdr(buf, (size_t)n, &pdu) != 0)
        {
            continue;
        }
        if (g_ldp_work_local && pdu.lsr_id == g_ldp_work_local->proto.lsr_id)
        {
            continue; /* 本端 hello 回环，丢弃 */
        }

        size_t pos = LDP_PDU_HEADER_SIZE;
        size_t end = (size_t)pdu.pdu_length + 4u;
        if (end > (size_t)n)
        {
            end = (size_t)n;
        }

        while (pos + LDP_MSG_HEADER_SIZE <= end)
        {
            ldp_msg_hdr_t mh;
            if (ldp_pkt_parse_msg_hdr(buf + pos, end - pos, &mh) != 0)
            {
                break;
            }
            const uint8_t *body = buf + pos + LDP_MSG_HEADER_SIZE;
            size_t body_len = (size_t)mh.msg_length > 4u ? (size_t)mh.msg_length - 4u : 0u;

            if (mh.msg_type == LDP_MSG_TYPE_HELLO)
            {
                ldp_hello_info_t info;
                if (ldp_pkt_parse_hello(body, body_len, &info) == 0 && !info.targeted)
                {
                    guint64 k = ldp_worker_adj_key(pdu.lsr_id, pdu.label_space);
                    ldp_adjacency_t *adj = (ldp_adjacency_t *)g_hash_table_lookup(g_ldp_work_local->adjacencies, &k);
                    if (!adj)
                    {
                        adj = g_malloc0(sizeof(*adj));
                        if (!adj)
                        {
                            break;
                        }
                        g_strlcpy(adj->ifname, iface->ifname, sizeof(adj->ifname));
                        adj->peer_lsr_id = pdu.lsr_id;
                        adj->peer_label_space = pdu.label_space;
                        guint64 *kheap = g_malloc(sizeof(*kheap));
                        *kheap = k;
                        g_hash_table_insert(g_ldp_work_local->adjacencies, kheap, adj);

                        char ip[16];
                        ldp_worker_format_lsr_id(pdu.lsr_id, ip, sizeof(ip));
                        LOG_INFO("LDP: adjacency learned %s:%u on %s", ip, pdu.label_space, iface->ifname);
                    }
                    adj->peer_transport_v4 = info.transport_v4;
                    adj->peer_hello_hold_ms = (uint32_t)info.hold_time_sec * 1000u;
                    adj->configuration_seq = info.configuration_seq;

                    /* 协商 hold = min(self, peer)，0(infinite) 视为最大 */
                    uint32_t self_hold = ldp_worker_effective_hold_ms(iface);
                    uint32_t peer_hold = adj->peer_hello_hold_ms ? adj->peer_hello_hold_ms : 0xFFFFFFFFu;
                    adj->neg_hold_ms = (self_hold && self_hold < peer_hold) ? self_hold : peer_hold;
                    adj->last_seen_msec = now_msec();

                    /* 触发 session 建立（M3）：transport_v4 缺失时退化为对端 LSR-ID */
                    uint32_t xport = adj->peer_transport_v4 ? adj->peer_transport_v4 : pdu.lsr_id;
                    ldp_session_on_adjacency_up(pdu.lsr_id, pdu.label_space, xport);
                }
            }

            pos += LDP_MSG_HEADER_SIZE + body_len;
        }
    }
}
