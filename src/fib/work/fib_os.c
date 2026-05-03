#include "fib_os.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

#define FIB_OS_NL_BUFSIZE 4096
#define FIB_OS_DUMP_BUFSIZE 65536

static uint8_t fib_route_protocol_to_rtproto(uint32_t protocol)
{
    switch (protocol)
    {
        case ROUTE_PROTOCOL_CONNECTED:
            return RTPROT_KERNEL;
        case ROUTE_PROTOCOL_BGP:
            return RTPROT_BGP;
        case ROUTE_PROTOCOL_OSPF:
            return RTPROT_OSPF;
        case ROUTE_PROTOCOL_ISIS:
            return RTPROT_ISIS;
        case ROUTE_PROTOCOL_STATIC:
        case ROUTE_PROTOCOL_BLACKHOLE:
        default:
            return RTPROT_STATIC;
    }
}

static void nl_add_attr(struct nlmsghdr *nlh, size_t maxlen, int type, const void *data, int datalen)
{
    int len = RTA_LENGTH(datalen);
    if (NLMSG_ALIGN(nlh->nlmsg_len) + (size_t)RTA_ALIGN(len) > maxlen)
    {
        LOG_WARN("fib_os: Netlink 属性超出缓冲区，跳过 type=%d", type);
        return;
    }

    struct rtattr *rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)len;
    memcpy(RTA_DATA(rta), data, (size_t)datalen);
    nlh->nlmsg_len = (unsigned int)(NLMSG_ALIGN(nlh->nlmsg_len) + (size_t)RTA_ALIGN(len));
}

static int nl_add_gateway_or_via(struct nlmsghdr *nlh, size_t maxlen, sa_family_t dst_family, const net_addr_t *gateway)
{
    if (!nlh || !gateway || (gateway->family != AF_INET && gateway->family != AF_INET6))
    {
        return ERRCODE_FAIL;
    }

    if (gateway->family == dst_family)
    {
        if (gateway->family == AF_INET)
        {
            nl_add_attr(nlh, maxlen, RTA_GATEWAY, &gateway->u.v4.s_addr, 4);
        }
        else
        {
            nl_add_attr(nlh, maxlen, RTA_GATEWAY, gateway->u.v6.s6_addr, 16);
        }
        return ERRCODE_SUCCESS;
    }

    uint8_t via_buf[sizeof(struct rtvia) + 16];
    memset(via_buf, 0, sizeof(via_buf));
    struct rtvia *via = (struct rtvia *)(void *)via_buf;
    via->rtvia_family = (unsigned short)gateway->family;

    int addr_len = 0;
    if (gateway->family == AF_INET)
    {
        memcpy(via_buf + sizeof(struct rtvia), &gateway->u.v4.s_addr, 4);
        addr_len = 4;
    }
    else
    {
        memcpy(via_buf + sizeof(struct rtvia), gateway->u.v6.s6_addr, 16);
        addr_len = 16;
    }

    nl_add_attr(nlh, maxlen, RTA_VIA, via_buf, (int)sizeof(struct rtvia) + addr_len);
    return ERRCODE_SUCCESS;
}

static int nl_exchange(struct nlmsghdr *nlh, int cmd)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        LOG_ERROR("fib_os: socket() 失败: %s", strerror(errno));
        return ERRCODE_FAIL;
    }

    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;

    if (sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&nladdr, sizeof(nladdr)) < 0)
    {
        LOG_ERROR("fib_os: sendto() 失败: %s", strerror(errno));
        close(fd);
        return ERRCODE_FAIL;
    }

    char ack_buf[FIB_OS_NL_BUFSIZE];
    ssize_t n = recv(fd, ack_buf, sizeof(ack_buf), 0);
    close(fd);

    if (n < 0)
    {
        LOG_ERROR("fib_os: recv() 失败: %s", strerror(errno));
        return ERRCODE_FAIL;
    }

    struct nlmsghdr *ack = (struct nlmsghdr *)(void *)ack_buf;
    if (ack->nlmsg_type == NLMSG_ERROR)
    {
        const struct nlmsgerr *err = (const struct nlmsgerr *)NLMSG_DATA(ack);
        if (err->error != 0 && !(cmd == RTM_DELROUTE && err->error == -ESRCH))
        {
            LOG_WARN("fib_os: Netlink 返回错误 %d: %s", -err->error, strerror(-err->error));
            return ERRCODE_FAIL;
        }
    }

    return ERRCODE_SUCCESS;
}

static int fib_os_route_send(int cmd, const fib_route_entry_t *route)
{
    if (!route)
    {
        return ERRCODE_FAIL;
    }

    char buf[FIB_OS_NL_BUFSIZE];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = (struct nlmsghdr *)(void *)buf;
    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nlh->nlmsg_type = (unsigned short)cmd;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    if (cmd == RTM_NEWROUTE)
    {
        nlh->nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
    }
    nlh->nlmsg_seq = 1;

    rtm->rtm_family = (unsigned char)route->prefix_addr.family;
    rtm->rtm_dst_len = route->prefix_len;
    rtm->rtm_table = RT_TABLE_MAIN;
    rtm->rtm_protocol = fib_route_protocol_to_rtproto(route->protocol);

    if (route->nh_type == FIB_NH_TYPE_BLACKHOLE)
    {
        rtm->rtm_type = RTN_BLACKHOLE;
        rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    }
    else if (net_addr_is_zero(&route->nexthop_addr) && route->out_ifindex != 0)
    {
        rtm->rtm_type = RTN_UNICAST;
        rtm->rtm_scope = RT_SCOPE_LINK;
    }
    else
    {
        rtm->rtm_type = RTN_UNICAST;
        rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    }

    if (route->prefix_addr.family == AF_INET)
    {
        nl_add_attr(nlh, sizeof(buf), RTA_DST, &route->prefix_addr.u.v4.s_addr, 4);
    }
    else if (route->prefix_addr.family == AF_INET6)
    {
        nl_add_attr(nlh, sizeof(buf), RTA_DST, route->prefix_addr.u.v6.s6_addr, 16);
    }
    else
    {
        return ERRCODE_FAIL;
    }

    if (route->out_ifindex != 0 && route->nh_type != FIB_NH_TYPE_BLACKHOLE)
    {
        nl_add_attr(nlh, sizeof(buf), RTA_OIF, &route->out_ifindex, (int)sizeof(route->out_ifindex));
    }

    if (route->nh_type == FIB_NH_TYPE_IP && !net_addr_is_zero(&route->nexthop_addr) &&
        nl_add_gateway_or_via(nlh, sizeof(buf), route->prefix_addr.family, &route->nexthop_addr) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    return nl_exchange(nlh, cmd);
}

int fib_os_route_install_ip(const fib_route_entry_t *route)
{
    if (route && (route->flags & FIB_ROUTE_FLAG_SKIP_OS) != 0)
    {
        return ERRCODE_SUCCESS;
    }
    return fib_os_route_send(RTM_NEWROUTE, route);
}

int fib_os_route_install_tunnel(const fib_route_entry_t *route, const fib_tunnel_entry_t *tunnel)
{
    if (!route || !tunnel || !tunnel->state || tunnel->label_count == 0)
    {
        return ERRCODE_FAIL;
    }

    fib_route_entry_t via_tunnel = *route;
    via_tunnel.nexthop_addr = tunnel->relay_addr;
    via_tunnel.out_ifindex = tunnel->out_ifindex;
    return fib_os_route_send(RTM_NEWROUTE, &via_tunnel);
}

int fib_os_route_withdraw(const fib_route_entry_t *route)
{
    if (route && (route->flags & FIB_ROUTE_FLAG_SKIP_OS) != 0)
    {
        return ERRCODE_SUCCESS;
    }
    return fib_os_route_send(RTM_DELROUTE, route);
}

static const char *os_table_str(uint8_t table)
{
    switch (table)
    {
        case RT_TABLE_MAIN:
            return "main";
        case RT_TABLE_LOCAL:
            return "local";
        case RT_TABLE_DEFAULT:
            return "default";
        default:
            return "other";
    }
}

static const char *os_type_str(uint8_t type)
{
    switch (type)
    {
        case RTN_UNICAST:
            return "unicast";
        case RTN_LOCAL:
            return "local";
        case RTN_BLACKHOLE:
            return "blackhole";
        case RTN_UNREACHABLE:
            return "unreachable";
        case RTN_PROHIBIT:
            return "prohibit";
        default:
            return "other";
    }
}

static const char *os_proto_str(uint8_t proto)
{
    switch (proto)
    {
        case RTPROT_KERNEL:
            return "kernel";
        case RTPROT_STATIC:
            return "static";
        case RTPROT_BGP:
            return "bgp";
        case RTPROT_OSPF:
            return "ospf";
        case RTPROT_ISIS:
            return "isis";
        case RTPROT_ZEBRA:
            return "zebra";
        case RTPROT_BOOT:
            return "boot";
        case RTPROT_REDIRECT:
            return "redirect";
        default:
            return "other";
    }
}

static void os_parse_route(struct nlmsghdr *nlh, GString *buf)
{
    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
    char dst_str[64];
    char gw_str[64] = "-";
    char oif_name[IF_NAMESIZE] = "-";
    uint32_t priority = 0;

    if (rtm->rtm_family == AF_INET)
    {
        inet_ntop(AF_INET, "\0\0\0\0", dst_str, sizeof(dst_str));
    }
    else
    {
        inet_ntop(AF_INET6, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", dst_str, sizeof(dst_str));
    }

    struct rtattr *rta = RTM_RTA(rtm);
    int rta_len = (int)RTM_PAYLOAD(nlh);
    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
    {
        switch (rta->rta_type)
        {
            case RTA_DST:
                inet_ntop(rtm->rtm_family, RTA_DATA(rta), dst_str, sizeof(dst_str));
                break;
            case RTA_GATEWAY:
                inet_ntop(rtm->rtm_family, RTA_DATA(rta), gw_str, sizeof(gw_str));
                break;
            case RTA_VIA:
            {
                const struct rtvia *via = (const struct rtvia *)RTA_DATA(rta);
                int via_payload_len = RTA_PAYLOAD(rta) - (int)sizeof(struct rtvia);
                const uint8_t *via_addr = (const uint8_t *)RTA_DATA(rta) + sizeof(struct rtvia);
                if (via_payload_len >= 4 && via->rtvia_family == AF_INET)
                {
                    inet_ntop(AF_INET, via_addr, gw_str, sizeof(gw_str));
                }
                else if (via_payload_len >= 16 && via->rtvia_family == AF_INET6)
                {
                    inet_ntop(AF_INET6, via_addr, gw_str, sizeof(gw_str));
                }
                break;
            }
            case RTA_OIF:
            {
                uint32_t idx;
                memcpy(&idx, RTA_DATA(rta), sizeof(idx));
                if (!if_indextoname(idx, oif_name))
                {
                    snprintf(oif_name, sizeof(oif_name), "if%u", idx);
                }
                break;
            }
            case RTA_PRIORITY:
                memcpy(&priority, RTA_DATA(rta), sizeof(priority));
                break;
            default:
                break;
        }
    }

    char prefix_str[80];
    snprintf(prefix_str, sizeof(prefix_str), "%s/%u", dst_str, rtm->rtm_dst_len);
    g_string_append_printf(buf, "%-7s %-10s %-26s %-20s %-14s %-8s %u\r\n", os_table_str(rtm->rtm_table),
                           os_type_str(rtm->rtm_type), prefix_str, gw_str, oif_name, os_proto_str(rtm->rtm_protocol),
                           priority);
}

int fib_os_show(GString *buf, sa_family_t family)
{
    if (!buf)
    {
        return ERRCODE_FAIL;
    }

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        LOG_ERROR("fib_os: socket() 失败: %s", strerror(errno));
        return ERRCODE_FAIL;
    }

    struct
    {
        struct nlmsghdr nlh;
        struct rtmsg rtm;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nlh.nlmsg_type = RTM_GETROUTE;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 3;
    req.rtm.rtm_family = (unsigned char)family;

    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;

    if (sendto(fd, &req, req.nlh.nlmsg_len, 0, (struct sockaddr *)&nladdr, sizeof(nladdr)) < 0)
    {
        LOG_ERROR("fib_os: sendto() 失败: %s", strerror(errno));
        close(fd);
        return ERRCODE_FAIL;
    }

    g_string_append_printf(buf,
                           "\r\n%-7s %-10s %-26s %-20s %-14s %-8s %s\r\n"
                           "------- ---------- -------------------------- "
                           "-------------------- -------------- -------- ------\r\n",
                           "Table", "Type", "Prefix", "Gateway", "Interface", "Proto", "Metric");

    char *recv_buf = (char *)g_malloc(FIB_OS_DUMP_BUFSIZE);
    if (!recv_buf)
    {
        close(fd);
        return ERRCODE_FAIL;
    }

    uint32_t count = 0;
    gboolean done = FALSE;
    while (!done)
    {
        int n = (int)recv(fd, recv_buf, FIB_OS_DUMP_BUFSIZE, 0);
        if (n < 0)
        {
            LOG_ERROR("fib_os: recv() 失败: %s", strerror(errno));
            break;
        }

        struct nlmsghdr *nlh = (struct nlmsghdr *)(void *)recv_buf;
        for (; NLMSG_OK(nlh, (unsigned int)n); nlh = NLMSG_NEXT(nlh, n))
        {
            if (nlh->nlmsg_type == NLMSG_DONE)
            {
                done = TRUE;
                break;
            }
            if (nlh->nlmsg_type == NLMSG_ERROR)
            {
                const struct nlmsgerr *err = (const struct nlmsgerr *)NLMSG_DATA(nlh);
                if (err->error != 0)
                {
                    LOG_WARN("fib_os: dump 错误 %d: %s", -err->error, strerror(-err->error));
                }
                done = TRUE;
                break;
            }
            if (nlh->nlmsg_type == RTM_NEWROUTE)
            {
                os_parse_route(nlh, buf);
                count++;
            }
        }
    }

    g_free(recv_buf);
    close(fd);
    g_string_append_printf(buf, "\r\nTotal %u route(s)\r\n", count);
    return ERRCODE_SUCCESS;
}
