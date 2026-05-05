/**
 * @file   dev_ping.c
 * @brief  Dev 模块自带 ICMP/ICMPv6 ping 会话实现
 * @author jhb
 * @date   2026-05-05
 *
 * 设计：
 * - 使用 SOCK_DGRAM/IPPROTO_ICMP / SOCK_DGRAM/IPPROTO_ICMPV6（datagram ICMP），
 *   无需 CAP_NET_RAW（容器内一般已开放 net.ipv4.ping_group_range）。
 * - 支持源地址绑定：socket 上 bind(src, port=0)。
 * - 状态机式：每次 dev_ping_next_line() 推进一步（header / 1..count probe / footer），
 *   阻塞 ≤ timeout_ms，不挂起 IPC 线程过久。
 */

#include "dev_ping.h"

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEV_PING_PAYLOAD_BYTES 56u
#define DEV_PING_HDR_BYTES 8u

typedef enum
{
    PING_PHASE_HEADER = 0,
    PING_PHASE_PROBE,
    PING_PHASE_FOOTER1,
    PING_PHASE_FOOTER2,
    PING_PHASE_DONE,
} ping_phase_t;

struct dev_ping_session
{
    int sockfd;
    int family;
    struct sockaddr_storage target_sa;
    socklen_t target_sa_len;
    bool has_source;
    uint16_t ident;
    int count;
    int timeout_ms;
    int sent;
    int received;
    int seq;
    long min_us;
    long max_us;
    long sum_us;
    char target_str[64];
    char source_str[64];
    ping_phase_t phase;
};

static uint16_t ping_csum(const void *buf, size_t len)
{
    const uint16_t *p = (const uint16_t *)buf;
    uint32_t sum = 0;
    while (len > 1)
    {
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
    {
        sum += *(const uint8_t *)p;
    }
    while (sum >> 16)
    {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000L + (long)(ts.tv_nsec / 1000L);
}

static int build_target_sockaddr(const net_addr_t *addr, struct sockaddr_storage *out, socklen_t *out_len)
{
    memset(out, 0, sizeof(*out));
    if (addr->family == AF_INET)
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)out;
        sin->sin_family = AF_INET;
        sin->sin_addr = addr->u.v4;
        *out_len = sizeof(*sin);
        return 0;
    }
    if (addr->family == AF_INET6)
    {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)out;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_addr = addr->u.v6;
        *out_len = sizeof(*sin6);
        return 0;
    }
    return -1;
}

dev_ping_session_t *dev_ping_start(const net_addr_t *target, const net_addr_t *source, int count, int timeout_ms,
                                   char *errmsg, size_t errmsg_len)
{
    if (!target || (target->family != AF_INET && target->family != AF_INET6))
    {
        if (errmsg)
        {
            g_strlcpy(errmsg, "invalid target address", errmsg_len);
        }
        return NULL;
    }
    if (source && source->family != target->family)
    {
        if (errmsg)
        {
            g_strlcpy(errmsg, "source/target address family mismatch", errmsg_len);
        }
        return NULL;
    }
    if (count <= 0)
    {
        count = 4;
    }
    if (timeout_ms <= 0)
    {
        timeout_ms = 2000;
    }

    int family = target->family;
    int proto = (family == AF_INET) ? IPPROTO_ICMP : IPPROTO_ICMPV6;
    int sockfd = socket(family, SOCK_DGRAM, proto);
    if (sockfd < 0)
    {
        if (errmsg)
        {
            g_snprintf(errmsg, errmsg_len, "socket(SOCK_DGRAM, %s) failed: %s", (family == AF_INET) ? "ICMP" : "ICMPV6",
                       strerror(errno));
        }
        return NULL;
    }

    bool has_source = false;
    if (source && source->family == family)
    {
        struct sockaddr_storage src_sa;
        socklen_t src_len = 0;
        if (build_target_sockaddr(source, &src_sa, &src_len) == 0)
        {
            if (bind(sockfd, (struct sockaddr *)&src_sa, src_len) != 0)
            {
                if (errmsg)
                {
                    g_snprintf(errmsg, errmsg_len, "bind source address failed: %s", strerror(errno));
                }
                close(sockfd);
                return NULL;
            }
            has_source = true;
        }
    }

    dev_ping_session_t *s = (dev_ping_session_t *)g_malloc0(sizeof(*s));
    if (!s)
    {
        close(sockfd);
        if (errmsg)
        {
            g_strlcpy(errmsg, "out of memory", errmsg_len);
        }
        return NULL;
    }
    s->sockfd = sockfd;
    s->family = family;
    s->ident = (uint16_t)(getpid() & 0xFFFFu);
    s->count = count;
    s->timeout_ms = timeout_ms;
    s->seq = 0;
    s->min_us = -1;
    s->max_us = -1;
    s->sum_us = 0;
    s->phase = PING_PHASE_HEADER;
    s->has_source = has_source;
    if (build_target_sockaddr(target, &s->target_sa, &s->target_sa_len) != 0)
    {
        close(sockfd);
        g_free(s);
        return NULL;
    }
    s->target_str[0] = '\0';
    net_addr_to_str(target, s->target_str, sizeof(s->target_str));
    if (s->target_str[0] == '\0')
    {
        g_strlcpy(s->target_str, "?", sizeof(s->target_str));
    }
    s->source_str[0] = '\0';
    if (has_source && source)
    {
        net_addr_to_str(source, s->source_str, sizeof(s->source_str));
        if (s->source_str[0] == '\0')
        {
            g_strlcpy(s->source_str, "?", sizeof(s->source_str));
        }
    }
    return s;
}

static int do_one_probe(dev_ping_session_t *s, char *out, size_t out_len)
{
    s->seq++;
    s->sent++;
    int seq = s->seq;
    int family = s->family;

    uint8_t pkt[DEV_PING_HDR_BYTES + DEV_PING_PAYLOAD_BYTES];
    memset(pkt, 0, sizeof(pkt));

    /* 填充时间戳到 payload 开头，便于 RTT 计算（不依赖时间戳协议） */
    long t_send = now_us();
    memcpy(pkt + DEV_PING_HDR_BYTES, &t_send, sizeof(t_send));
    /* 其余 payload 填充递增字节，便于抓包对照 */
    for (size_t i = sizeof(t_send); i < DEV_PING_PAYLOAD_BYTES; ++i)
    {
        pkt[DEV_PING_HDR_BYTES + i] = (uint8_t)(0x10u + i);
    }

    if (family == AF_INET)
    {
        struct icmphdr *h = (struct icmphdr *)pkt;
        h->type = ICMP_ECHO;
        h->code = 0;
        h->un.echo.id = htons(s->ident);
        h->un.echo.sequence = htons((uint16_t)seq);
        h->checksum = 0;
        h->checksum = ping_csum(pkt, sizeof(pkt));
    }
    else
    {
        struct icmp6_hdr *h = (struct icmp6_hdr *)pkt;
        h->icmp6_type = ICMP6_ECHO_REQUEST;
        h->icmp6_code = 0;
        h->icmp6_id = htons(s->ident);
        h->icmp6_seq = htons((uint16_t)seq);
        /* ICMPv6 校验和由内核基于 IPv6 伪首部计算 */
        h->icmp6_cksum = 0;
    }

    ssize_t n = sendto(s->sockfd, pkt, sizeof(pkt), 0, (struct sockaddr *)&s->target_sa, s->target_sa_len);
    if (n < 0)
    {
        g_snprintf(out, out_len, "send icmp_seq=%d failed: %s", seq, strerror(errno));
        return 1;
    }

    /* 等待回包，过滤匹配的 seq/id；超时则上报 timeout */
    long deadline = now_us() + (long)s->timeout_ms * 1000L;
    for (;;)
    {
        long remain_us = deadline - now_us();
        if (remain_us <= 0)
        {
            g_snprintf(out, out_len, "Request timeout for icmp_seq=%d", seq);
            return 1;
        }
        int poll_ms = (int)(remain_us / 1000L);
        if (poll_ms < 1)
        {
            poll_ms = 1;
        }
        struct pollfd pfd = {.fd = s->sockfd, .events = POLLIN, .revents = 0};
        int pr = poll(&pfd, 1, poll_ms);
        if (pr <= 0)
        {
            if (pr == 0)
            {
                g_snprintf(out, out_len, "Request timeout for icmp_seq=%d", seq);
                return 1;
            }
            if (errno == EINTR)
            {
                continue;
            }
            g_snprintf(out, out_len, "poll failed: %s", strerror(errno));
            return 1;
        }

        uint8_t rbuf[2048];
        struct sockaddr_storage from;
        socklen_t fromlen = sizeof(from);
        ssize_t rn = recvfrom(s->sockfd, rbuf, sizeof(rbuf), 0, (struct sockaddr *)&from, &fromlen);
        if (rn < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            g_snprintf(out, out_len, "recvfrom failed: %s", strerror(errno));
            return 1;
        }
        long t_recv = now_us();

        /* SOCK_DGRAM/IPPROTO_ICMP{,V6} 收到的数据已剥离 IP 头，从 ICMP 头开始 */
        uint16_t got_seq = 0;
        uint8_t got_type = 0;
        if (family == AF_INET)
        {
            if ((size_t)rn < sizeof(struct icmphdr))
            {
                continue;
            }
            const struct icmphdr *h = (const struct icmphdr *)rbuf;
            got_seq = ntohs(h->un.echo.sequence);
            got_type = h->type;
        }
        else
        {
            if ((size_t)rn < sizeof(struct icmp6_hdr))
            {
                continue;
            }
            const struct icmp6_hdr *h = (const struct icmp6_hdr *)rbuf;
            got_seq = ntohs(h->icmp6_seq);
            got_type = h->icmp6_type;
        }
        /*
         * SOCK_DGRAM ICMP sockets are already demultiplexed by the kernel.
         * Linux may manage the ICMP identifier itself, so only use sequence
         * and type here to avoid discarding valid echo replies.
         */
        if (got_seq != (uint16_t)seq)
        {
            continue;
        }
        bool is_reply = (family == AF_INET) ? (got_type == ICMP_ECHOREPLY) : (got_type == ICMP6_ECHO_REPLY);
        if (!is_reply)
        {
            g_snprintf(out, out_len, "From %s: icmp_seq=%d type=%u", s->target_str, seq, (unsigned)got_type);
            return 1;
        }

        long rtt_us = t_recv - t_send;
        if (rtt_us < 0)
        {
            rtt_us = 0;
        }
        if (s->min_us < 0 || rtt_us < s->min_us)
        {
            s->min_us = rtt_us;
        }
        if (rtt_us > s->max_us)
        {
            s->max_us = rtt_us;
        }
        s->sum_us += rtt_us;
        s->received++;

        char from_str[64];
        from_str[0] = '\0';
        if (family == AF_INET)
        {
            const struct sockaddr_in *fin = (const struct sockaddr_in *)&from;
            inet_ntop(AF_INET, &fin->sin_addr, from_str, sizeof(from_str));
        }
        else
        {
            const struct sockaddr_in6 *fin6 = (const struct sockaddr_in6 *)&from;
            inet_ntop(AF_INET6, &fin6->sin6_addr, from_str, sizeof(from_str));
        }

        g_snprintf(out, out_len, "%zu bytes from %s: icmp_seq=%d time=%ld.%03ld ms",
                   (size_t)(DEV_PING_HDR_BYTES + DEV_PING_PAYLOAD_BYTES), from_str, seq, rtt_us / 1000L,
                   (rtt_us % 1000L));
        return 1;
    }
}

int dev_ping_next_line(dev_ping_session_t *s, char *out, size_t out_len)
{
    if (!s || !out || out_len == 0)
    {
        return 0;
    }
    switch (s->phase)
    {
        case PING_PHASE_HEADER:
            if (s->has_source)
            {
                g_snprintf(out, out_len, "PING %s from %s: %u data bytes", s->target_str, s->source_str,
                           DEV_PING_PAYLOAD_BYTES);
            }
            else
            {
                g_snprintf(out, out_len, "PING %s: %u data bytes", s->target_str, DEV_PING_PAYLOAD_BYTES);
            }
            s->phase = PING_PHASE_PROBE;
            return 1;
        case PING_PHASE_PROBE:
        {
            int rc = do_one_probe(s, out, out_len);
            if (s->seq >= s->count)
            {
                s->phase = PING_PHASE_FOOTER1;
            }
            return rc;
        }
        case PING_PHASE_FOOTER1:
        {
            int loss = (s->sent > 0) ? (int)((double)(s->sent - s->received) * 100.0 / (double)s->sent) : 0;
            g_snprintf(out, out_len, "--- %s ping statistics ---", s->target_str);
            s->phase = PING_PHASE_FOOTER2;
            (void)loss; /* 主统计行放在 FOOTER2 */
            return 1;
        }
        case PING_PHASE_FOOTER2:
        {
            int loss = (s->sent > 0) ? (int)((double)(s->sent - s->received) * 100.0 / (double)s->sent) : 0;
            if (s->received > 0)
            {
                long avg_us = s->sum_us / s->received;
                g_snprintf(out, out_len,
                           "%d packets transmitted, %d received, %d%% packet loss, "
                           "rtt min/avg/max = %ld.%03ld/%ld.%03ld/%ld.%03ld ms",
                           s->sent, s->received, loss, s->min_us / 1000L, s->min_us % 1000L, avg_us / 1000L,
                           avg_us % 1000L, s->max_us / 1000L, s->max_us % 1000L);
            }
            else
            {
                g_snprintf(out, out_len, "%d packets transmitted, %d received, %d%% packet loss", s->sent, s->received,
                           loss);
            }
            s->phase = PING_PHASE_DONE;
            return 1;
        }
        case PING_PHASE_DONE:
        default:
            return 0;
    }
}

void dev_ping_close(dev_ping_session_t *s)
{
    if (!s)
    {
        return;
    }
    if (s->sockfd >= 0)
    {
        close(s->sockfd);
        s->sockfd = -1;
    }
    g_free(s);
}
