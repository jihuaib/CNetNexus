/**
 * @file   route_os.c
 * @brief  通过 Netlink RTM_NEWROUTE/DELROUTE 向 Linux 内核下发/撤销路由
 * @author jhb
 * @date   2026/03/22
 */
#include "route_os.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <errno.h>
#include <glib.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "net_addr.h"
#include "route.h"

/* Netlink 请求/响应缓冲区大小 */
#define ROUTE_OS_NL_BUFSIZE 4096

/* ============================================================================
 * 内部辅助：添加 Netlink 属性
 * ============================================================================ */

static void nl_add_attr(struct nlmsghdr *nlh, size_t maxlen, int type, const void *data, int datalen)
{
    int len = RTA_LENGTH(datalen);
    if (NLMSG_ALIGN(nlh->nlmsg_len) + (size_t)RTA_ALIGN(len) > maxlen)
    {
        LOG_WARN("route_os: Netlink 属性超出缓冲区，跳过 type=%d", type);
        return;
    }
    struct rtattr *rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)len;
    memcpy(RTA_DATA(rta), data, (size_t)datalen);
    nlh->nlmsg_len = (unsigned int)(NLMSG_ALIGN(nlh->nlmsg_len) + (size_t)RTA_ALIGN(len));
}

/* ============================================================================
 * 内部辅助：Netlink 报文发送与 ACK 接收
 * ============================================================================ */

static int nl_exchange(struct nlmsghdr *nlh, int cmd)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        LOG_ERROR("route_os: socket() 失败: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;

    if (sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&nladdr, sizeof(nladdr)) < 0)
    {
        LOG_ERROR("route_os: sendto() 失败: %s", strerror(errno));
        close(fd);
        return -1;
    }

    char ack_buf[ROUTE_OS_NL_BUFSIZE];
    ssize_t n = recv(fd, ack_buf, sizeof(ack_buf), 0);
    close(fd);

    if (n < 0)
    {
        LOG_ERROR("route_os: recv() 失败: %s", strerror(errno));
        return -1;
    }

    struct nlmsghdr *ack = (struct nlmsghdr *)(void *)ack_buf;
    if (ack->nlmsg_type == NLMSG_ERROR)
    {
        const struct nlmsgerr *err = (const struct nlmsgerr *)NLMSG_DATA(ack);
        /* ESRCH on delete = 路由不存在，正常忽略 */
        if (err->error != 0 && !(cmd == RTM_DELROUTE && err->error == -ESRCH))
        {
            LOG_WARN("route_os: Netlink 返回错误 %d: %s", -err->error, strerror(-err->error));
            return -1;
        }
    }

    return 0;
}

/* ============================================================================
 * 核心发送函数：RT_TABLE_MAIN
 * ============================================================================ */

static int route_os_send(int cmd, const route_msg_entry_t *entry)
{
    if (!entry)
    {
        return -1;
    }

    int is_non_connected = (entry->protocol != ROUTE_PROTOCOL_CONNECTED && entry->protocol != ROUTE_PROTOCOL_BLACKHOLE);
    net_addr_t effective_gateway = entry->nexthop_addr;
    uint32_t effective_oif = entry->out_ifindex;

    char buf[ROUTE_OS_NL_BUFSIZE];
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

    /* 填充 rtmsg 基本字段 */
    rtm->rtm_family = (unsigned char)entry->prefix_addr.family;
    rtm->rtm_dst_len = entry->prefix_len;
    rtm->rtm_src_len = 0;
    rtm->rtm_tos = 0;
    rtm->rtm_table = RT_TABLE_MAIN;
    switch (entry->protocol)
    {
        case ROUTE_PROTOCOL_CONNECTED:
            rtm->rtm_protocol = RTPROT_KERNEL;
            break;
        case ROUTE_PROTOCOL_BGP:
            rtm->rtm_protocol = RTPROT_BGP;
            break;
        case ROUTE_PROTOCOL_OSPF:
            rtm->rtm_protocol = RTPROT_OSPF;
            break;
        default:
            rtm->rtm_protocol = RTPROT_STATIC;
            break;
    }
    rtm->rtm_flags = 0;

    /* 路由类型与 scope */
    if (entry->protocol == ROUTE_PROTOCOL_BLACKHOLE)
    {
        rtm->rtm_type = RTN_BLACKHOLE;
        rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    }
    else if (entry->protocol == ROUTE_PROTOCOL_CONNECTED && entry->out_ifindex != 0)
    {
        rtm->rtm_type = RTN_UNICAST;
        rtm->rtm_scope = RT_SCOPE_LINK;
    }
    else
    {
        rtm->rtm_type = RTN_UNICAST;
        rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    }

    /* RTA_DST：目标前缀 */
    if (entry->prefix_addr.family == AF_INET)
    {
        nl_add_attr(nlh, sizeof(buf), RTA_DST, &entry->prefix_addr.u.v4.s_addr, 4);
    }
    else if (entry->prefix_addr.family == AF_INET6)
    {
        nl_add_attr(nlh, sizeof(buf), RTA_DST, entry->prefix_addr.u.v6.s6_addr, 16);
    }
    else
    {
        return -1;
    }

    /* RTA_OIF：出接口（直连路由） */
    if (effective_oif != 0 && entry->protocol != ROUTE_PROTOCOL_BLACKHOLE)
    {
        nl_add_attr(nlh, sizeof(buf), RTA_OIF, &effective_oif, (int)sizeof(uint32_t));
    }

    /*
     * RTA_PREFSRC：直连路由优选源地址。
     *
     * IPv4: 保留，避免内核从管理口地址选源。
     * IPv6: 不下发该属性。当前模型下 IPv6 地址通过 LOCAL /128 路由表达，
     *       未在网卡上显式配置 addr；对 connected /64 带 PREFSRC 会被内核
     *       以 EINVAL 拒绝，导致直连路由安装失败。
     */
    if (entry->protocol == ROUTE_PROTOCOL_CONNECTED && entry->source_addr.family == AF_INET &&
        entry->prefix_addr.family == AF_INET)
    {
        uint32_t zero = 0;
        if (memcmp(&entry->source_addr.u.v4.s_addr, &zero, sizeof(zero)) != 0)
        {
            nl_add_attr(nlh, sizeof(buf), RTA_PREFSRC, &entry->source_addr.u.v4.s_addr, 4);
        }
    }

    /* RTA_GATEWAY：网关（非直连、非黑洞路由且 nexthop 非零） */
    if (is_non_connected && !net_addr_is_zero(&effective_gateway))
    {
        if (effective_gateway.family == AF_INET)
        {
            nl_add_attr(nlh, sizeof(buf), RTA_GATEWAY, &effective_gateway.u.v4.s_addr, 4);
        }
        else if (effective_gateway.family == AF_INET6)
        {
            nl_add_attr(nlh, sizeof(buf), RTA_GATEWAY, effective_gateway.u.v6.s6_addr, 16);
        }
        else
        {
            return -1;
        }
    }

    return nl_exchange(nlh, cmd);
}

/* ============================================================================
 * 公共 API
 * ============================================================================ */

int route_os_install(const route_msg_entry_t *entry)
{
    /*
     * CONNECTED 路由由 IF 模块通过 ifaddr 驱动内核自动生成（main/local），
     * route 模块仅维护内存 RIB，不再显式下发内核 connected 路由。
     */
    if (entry && entry->protocol == ROUTE_PROTOCOL_CONNECTED)
    {
        return 0;
    }

    return route_os_send(RTM_NEWROUTE, entry);
}

int route_os_withdraw(const route_msg_entry_t *entry)
{
    if (entry && entry->protocol == ROUTE_PROTOCOL_CONNECTED)
    {
        return 0;
    }

    return route_os_send(RTM_DELROUTE, entry);
}

/* ============================================================================
 * 内核路由表查询（RTM_GETROUTE dump）
 * ============================================================================ */

/* 接收缓冲区：64KB 足以容纳数百条路由的 dump 响应 */
#define ROUTE_OS_DUMP_BUFSIZE 65536

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

    /* 默认值 */
    char dst_str[64];
    char gw_str[64] = "-";
    char oif_name[IF_NAMESIZE] = "-";
    uint32_t priority = 0;

    /* 默认目标：全零地址 */
    if (rtm->rtm_family == AF_INET)
    {
        inet_ntop(AF_INET, "\0\0\0\0", dst_str, sizeof(dst_str));
    }
    else
    {
        inet_ntop(AF_INET6, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", dst_str, sizeof(dst_str));
    }

    /* 遍历 RTA 属性 */
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

int route_os_show(GString *buf, sa_family_t family)
{
    if (!buf)
    {
        return -1;
    }

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        LOG_ERROR("route_os: socket() 失败: %s", strerror(errno));
        return -1;
    }

    /* 构造 RTM_GETROUTE dump 请求 */
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
        LOG_ERROR("route_os: sendto() 失败: %s", strerror(errno));
        close(fd);
        return -1;
    }

    g_string_append_printf(buf,
                           "\r\n%-7s %-10s %-26s %-20s %-14s %-8s %s\r\n"
                           "------- ---------- -------------------------- "
                           "-------------------- -------------- -------- ------\r\n",
                           "Table", "Type", "Prefix", "Gateway", "Interface", "Proto", "Metric");

    /* 接收 dump 响应（可能跨多个 recv 报文） */
    char *recv_buf = (char *)g_malloc(ROUTE_OS_DUMP_BUFSIZE);
    if (!recv_buf)
    {
        close(fd);
        return -1;
    }

    uint32_t count = 0;
    gboolean done = FALSE;
    while (!done)
    {
        int n = (int)recv(fd, recv_buf, ROUTE_OS_DUMP_BUFSIZE, 0);
        if (n < 0)
        {
            LOG_ERROR("route_os: recv() 失败: %s", strerror(errno));
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
                    LOG_WARN("route_os: dump 错误 %d: %s", -err->error, strerror(-err->error));
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
    return 0;
}
