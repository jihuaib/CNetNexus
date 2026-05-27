#include "fib_os.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <linux/lwtunnel.h>
#include <linux/mpls.h>
#include <linux/mpls_iptunnel.h>
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
#include "mpls_config.h"
#include "net_addr.h"
#include "route.h"
#include "tunnel.h"
#include "vrf.h"

#define FIB_OS_NL_BUFSIZE 4096
#define FIB_OS_DUMP_BUFSIZE 65536
#define FIB_OS_MPLS_DST_LEN 20

#ifndef AF_MPLS
#    define AF_MPLS 28
#endif

static uint32_t g_mpls_platform_labels;
static gboolean g_mpls_input = TRUE;
static gboolean g_mpls_configured;

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

static struct rtattr *nl_attr_nest_start(struct nlmsghdr *nlh, size_t maxlen, int type)
{
    int len = RTA_LENGTH(0);
    if (NLMSG_ALIGN(nlh->nlmsg_len) + (size_t)RTA_ALIGN(len) > maxlen)
    {
        LOG_WARN("fib_os: Netlink nested 属性超出缓冲区，跳过 type=%d", type);
        return NULL;
    }

    struct rtattr *rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)len;
    nlh->nlmsg_len = (unsigned int)(NLMSG_ALIGN(nlh->nlmsg_len) + (size_t)RTA_ALIGN(len));
    return rta;
}

static void nl_attr_nest_end(struct nlmsghdr *nlh, struct rtattr *nest)
{
    if (!nlh || !nest)
    {
        return;
    }
    nest->rta_len = (unsigned short)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len) - (char *)nest);
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

static int nl_add_mpls_encap(struct nlmsghdr *nlh, size_t maxlen, const fib_tunnel_entry_t *tunnel)
{
    if (!nlh || !tunnel || tunnel->label_count == 0 || tunnel->label_count > FIB_MAX_LABEL_STACK)
    {
        return ERRCODE_FAIL;
    }

    struct mpls_label stack[FIB_MAX_LABEL_STACK];
    memset(stack, 0, sizeof(stack));
    for (uint8_t i = 0; i < tunnel->label_count; i++)
    {
        if (tunnel->labels[i] > 0xFFFFFu)
        {
            return ERRCODE_FAIL;
        }
        uint32_t entry = tunnel->labels[i] << MPLS_LS_LABEL_SHIFT;
        if (i == tunnel->label_count - 1u)
        {
            entry |= (1u << MPLS_LS_S_SHIFT);
        }
        stack[i].entry = htonl(entry);
    }

    uint16_t encap_type = LWTUNNEL_ENCAP_MPLS;
    nl_add_attr(nlh, maxlen, RTA_ENCAP_TYPE, &encap_type, (int)sizeof(encap_type));

    struct rtattr *encap = nl_attr_nest_start(nlh, maxlen, RTA_ENCAP);
    if (!encap)
    {
        return ERRCODE_FAIL;
    }
    nl_add_attr(nlh, maxlen, MPLS_IPTUNNEL_DST, stack, (int)(sizeof(stack[0]) * tunnel->label_count));
    nl_attr_nest_end(nlh, encap);
    return ERRCODE_SUCCESS;
}

static int encode_mpls_label(uint32_t label, gboolean bos, struct mpls_label *out)
{
    if (!out || label > 0xFFFFFu)
    {
        return ERRCODE_FAIL;
    }

    uint32_t entry = label << MPLS_LS_LABEL_SHIFT;
    if (bos)
    {
        entry |= (1u << MPLS_LS_S_SHIFT);
    }
    out->entry = htonl(entry);
    return ERRCODE_SUCCESS;
}

static int nl_add_mpls_label_stack(struct nlmsghdr *nlh, size_t maxlen, int attr_type, uint8_t count,
                                   const uint32_t *labels)
{
    if (!nlh || !labels || count == 0 || count > FIB_MAX_LABEL_STACK)
    {
        return ERRCODE_FAIL;
    }

    struct mpls_label stack[FIB_MAX_LABEL_STACK];
    memset(stack, 0, sizeof(stack));
    for (uint8_t i = 0; i < count; i++)
    {
        if (encode_mpls_label(labels[i], i == count - 1u, &stack[i]) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }

    nl_add_attr(nlh, maxlen, attr_type, stack, (int)(sizeof(stack[0]) * count));
    return ERRCODE_SUCCESS;
}

static int fib_os_write_uint_file(const char *path, uint32_t value)
{
    FILE *fp = fopen(path, "w");
    if (!fp)
    {
        LOG_WARN("fib_os: open %s failed: %s (kernel MPLS support not loaded?)", path, strerror(errno));
        return ERRCODE_FAIL;
    }
    int rc = (fprintf(fp, "%u\n", value) > 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    if (rc != ERRCODE_SUCCESS)
    {
        LOG_WARN("fib_os: write %s=%u failed: %s", path, value, strerror(errno));
    }
    fclose(fp);
    return rc;
}

static int fib_os_apply_mpls_platform_labels(uint32_t labels)
{
    if (labels == 0u)
    {
        return ERRCODE_SUCCESS;
    }

    return fib_os_write_uint_file("/proc/sys/net/mpls/platform_labels", labels);
}

static int fib_os_apply_mpls_input(gboolean enabled)
{
    DIR *dir = opendir("/proc/sys/net/mpls/conf");
    if (!dir)
    {
        LOG_WARN("fib_os: opendir /proc/sys/net/mpls/conf failed: %s "
                 "(kernel mpls_router module not loaded; MPLS forwarding will not work)",
                 strerror(errno));
        return ERRCODE_FAIL;
    }

    struct dirent *de = NULL;
    while ((de = readdir(dir)) != NULL)
    {
        if (de->d_name[0] == '.')
        {
            continue;
        }

        char path[256];
        int n = snprintf(path, sizeof(path), "/proc/sys/net/mpls/conf/%s/input", de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
        {
            continue;
        }
        (void)fib_os_write_uint_file(path, enabled ? 1u : 0u);
    }
    closedir(dir);
    return ERRCODE_SUCCESS;
}

static void fib_os_prepare_mpls_platform_labels(uint32_t label)
{
    uint32_t want = (g_mpls_configured && g_mpls_platform_labels != 0u) ? g_mpls_platform_labels : 0u;
    uint32_t min_want = (label >= 0xFFFFFu) ? NN_MPLS_PLATFORM_LABELS_MAX : label + 1u;
    if (want < min_want)
    {
        want = min_want;
    }
    uint32_t current = 0u;

    FILE *fp = fopen("/proc/sys/net/mpls/platform_labels", "r");
    if (fp)
    {
        if (fscanf(fp, "%u", &current) != 1)
        {
            current = 0u;
        }
        fclose(fp);
    }
    if (current >= want)
    {
        return;
    }

    fib_os_write_uint_file("/proc/sys/net/mpls/platform_labels", want);
}

int fib_os_mpls_configure(uint32_t platform_labels, gboolean mpls_input)
{
    if (platform_labels == 0u || platform_labels > NN_MPLS_PLATFORM_LABELS_MAX)
    {
        return ERRCODE_FAIL;
    }

    g_mpls_platform_labels = platform_labels;
    g_mpls_input = mpls_input;
    g_mpls_configured = TRUE;

    int labels_rc = fib_os_apply_mpls_platform_labels(g_mpls_platform_labels);
    int input_rc = fib_os_apply_mpls_input(g_mpls_input);
    if (labels_rc != ERRCODE_SUCCESS || input_rc != ERRCODE_SUCCESS)
    {
        LOG_WARN("fib_os: MPLS kernel config NOT fully applied (platform_labels=%u %s, mpls_input=%s %s); "
                 "load mpls_router kernel module and grant NET_ADMIN to enable MPLS forwarding",
                 g_mpls_platform_labels, labels_rc == ERRCODE_SUCCESS ? "ok" : "FAIL", g_mpls_input ? "true" : "false",
                 input_rc == ERRCODE_SUCCESS ? "ok" : "FAIL");
        return ERRCODE_FAIL;
    }
    LOG_INFO("fib_os: MPLS kernel config applied platform_labels=%u mpls_input=%s", g_mpls_platform_labels,
             g_mpls_input ? "true" : "false");
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

static int fib_os_route_send(int cmd, const fib_route_entry_t *route, const fib_tunnel_entry_t *tunnel)
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
    rtm->rtm_protocol = fib_route_protocol_to_rtproto(route->protocol);

    /* 路由表 ID 解析：公网 VRF → MAIN；私网 VRF → L3VRF table_id（从 VRF 缓存查询） */
    uint32_t table_id = RT_TABLE_MAIN;
    if (route->vrf_id != VRF_PUBLIC_VRF_ID)
    {
        const vrf_api_cache_entry_t *vrf = vrf_api_cache_lookup(route->vrf_id);
        if (!vrf || vrf->l3vrf_table_id == 0)
        {
            LOG_WARN("[fib_os] VRF id=%u not in cache or no l3vrf_table_id, route not programmed", route->vrf_id);
            return ERRCODE_FAIL;
        }
        table_id = vrf->l3vrf_table_id;
    }

    if (table_id < 256)
    {
        rtm->rtm_table = (unsigned char)table_id;
    }
    else
    {
        /* 大表 ID 必须走 RTA_TABLE 属性 */
        rtm->rtm_table = RT_TABLE_UNSPEC;
    }

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

    /* 写 RTA_TABLE（任何 VRF 都补一份，保持与内核 dump 行为一致） */
    nl_add_attr(nlh, sizeof(buf), RTA_TABLE, &table_id, (int)sizeof(table_id));

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

    if (cmd == RTM_NEWROUTE && route->out_ifindex != 0 && route->nh_type != FIB_NH_TYPE_BLACKHOLE &&
        !net_addr_is_zero(&route->nexthop_addr))
    {
        rtm->rtm_flags |= RTNH_F_ONLINK;
    }

    if (route->nh_type == FIB_NH_TYPE_IP && !net_addr_is_zero(&route->nexthop_addr) &&
        nl_add_gateway_or_via(nlh, sizeof(buf), route->prefix_addr.family, &route->nexthop_addr) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (cmd == RTM_NEWROUTE && route->nh_type == FIB_NH_TYPE_TUNNEL)
    {
        if (!tunnel || nl_add_mpls_encap(nlh, sizeof(buf), tunnel) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
        if (!net_addr_is_zero(&route->nexthop_addr) &&
            nl_add_gateway_or_via(nlh, sizeof(buf), route->prefix_addr.family, &route->nexthop_addr) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }

    return nl_exchange(nlh, cmd);
}

static int fib_os_mpls_route_send(int cmd, const fib_ilm_entry_t *ilm)
{
    if (!ilm || ilm->in_label == 0 || ilm->in_label > 0xFFFFFu)
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
    nlh->nlmsg_seq = 2;

    rtm->rtm_family = AF_MPLS;
    rtm->rtm_dst_len = FIB_OS_MPLS_DST_LEN;
    rtm->rtm_table = RT_TABLE_MAIN;
    rtm->rtm_protocol = RTPROT_STATIC;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    rtm->rtm_type = (ilm->action == TUNNEL_ACTION_DROP) ? RTN_BLACKHOLE : RTN_UNICAST;

    struct mpls_label in_label;
    if (encode_mpls_label(ilm->in_label, TRUE, &in_label) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    nl_add_attr(nlh, sizeof(buf), RTA_DST, &in_label, (int)sizeof(in_label));

    if (cmd == RTM_NEWROUTE)
    {
        if (ilm->action == TUNNEL_ACTION_SWAP)
        {
            if (nl_add_mpls_label_stack(nlh, sizeof(buf), RTA_NEWDST, ilm->label_count, ilm->labels) != ERRCODE_SUCCESS)
            {
                return ERRCODE_FAIL;
            }
        }
        else if (ilm->action != TUNNEL_ACTION_POP && ilm->action != TUNNEL_ACTION_PHP &&
                 ilm->action != TUNNEL_ACTION_DROP)
        {
            return ERRCODE_FAIL;
        }

        if (ilm->out_ifindex != 0 && ilm->action != TUNNEL_ACTION_DROP)
        {
            nl_add_attr(nlh, sizeof(buf), RTA_OIF, &ilm->out_ifindex, (int)sizeof(ilm->out_ifindex));
        }
        if (!net_addr_is_zero(&ilm->relay_addr) &&
            nl_add_gateway_or_via(nlh, sizeof(buf), AF_MPLS, &ilm->relay_addr) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }

    return nl_exchange(nlh, cmd);
}

int fib_os_route_install_ip(const fib_route_entry_t *route)
{
    if (route && (route->flags & FIB_ROUTE_FLAG_SKIP_OS) != 0)
    {
        return ERRCODE_SUCCESS;
    }
    return fib_os_route_send(RTM_NEWROUTE, route, NULL);
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
    return fib_os_route_send(RTM_NEWROUTE, &via_tunnel, tunnel);
}

int fib_os_route_withdraw(const fib_route_entry_t *route)
{
    if (route && (route->flags & FIB_ROUTE_FLAG_SKIP_OS) != 0)
    {
        return ERRCODE_SUCCESS;
    }
    return fib_os_route_send(RTM_DELROUTE, route, NULL);
}

int fib_os_ilm_install(const fib_ilm_entry_t *ilm)
{
    if (!ilm || !ilm->state)
    {
        return ERRCODE_FAIL;
    }
    fib_os_prepare_mpls_platform_labels(ilm->in_label);
    fib_os_apply_mpls_input(g_mpls_input);
    return fib_os_mpls_route_send(RTM_NEWROUTE, ilm);
}

int fib_os_ilm_withdraw(const fib_ilm_entry_t *ilm)
{
    return fib_os_mpls_route_send(RTM_DELROUTE, ilm);
}

static void os_table_format(uint32_t table, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    switch (table)
    {
        case RT_TABLE_MAIN:
            g_strlcpy(buf, "main", sz);
            return;
        case RT_TABLE_LOCAL:
            g_strlcpy(buf, "local", sz);
            return;
        case RT_TABLE_DEFAULT:
            g_strlcpy(buf, "default", sz);
            return;
        default:
            snprintf(buf, sz, "%u", table);
            return;
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

static void os_parse_mpls_encap(const struct rtattr *encap_attr, char *buf, size_t sz)
{
    if (!encap_attr || !buf || sz == 0)
    {
        return;
    }

    int len = RTA_PAYLOAD(encap_attr);
    struct rtattr *rta = (struct rtattr *)RTA_DATA(encap_attr);
    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
    {
        if (rta->rta_type != MPLS_IPTUNNEL_DST)
        {
            continue;
        }

        int count = RTA_PAYLOAD(rta) / (int)sizeof(struct mpls_label);
        const struct mpls_label *labels = (const struct mpls_label *)RTA_DATA(rta);
        size_t used = 0;
        int n = snprintf(buf, sz, "mpls[");
        if (n < 0 || (size_t)n >= sz)
        {
            return;
        }
        used = (size_t)n;

        for (int i = 0; i < count; i++)
        {
            uint32_t entry = ntohl(labels[i].entry);
            uint32_t label = (entry & MPLS_LS_LABEL_MASK) >> MPLS_LS_LABEL_SHIFT;
            n = snprintf(buf + used, sz - used, "%s%u", (i == 0) ? "" : ",", label);
            if (n < 0 || (size_t)n >= sz - used)
            {
                snprintf(buf + used, sz - used, "...");
                return;
            }
            used += (size_t)n;
        }

        snprintf(buf + used, sz - used, "]");
        return;
    }
}

static void os_parse_mpls_stack(const struct rtattr *stack_attr, char *buf, size_t sz)
{
    if (!stack_attr || !buf || sz == 0)
    {
        return;
    }

    int count = RTA_PAYLOAD(stack_attr) / (int)sizeof(struct mpls_label);
    const struct mpls_label *labels = (const struct mpls_label *)RTA_DATA(stack_attr);
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < count; i++)
    {
        uint32_t entry = ntohl(labels[i].entry);
        uint32_t label = (entry & MPLS_LS_LABEL_MASK) >> MPLS_LS_LABEL_SHIFT;
        int n = snprintf(buf + used, sz - used, "%s%u", (i == 0) ? "" : ",", label);
        if (n < 0 || (size_t)n >= sz - used)
        {
            snprintf(buf + used, sz - used, "...");
            return;
        }
        used += (size_t)n;
    }
}

static gboolean os_if_master_matches(uint32_t ifindex, uint32_t expected_master_ifindex)
{
    if (ifindex == 0)
    {
        return FALSE;
    }

    char ifname[IF_NAMESIZE];
    if (!if_indextoname(ifindex, ifname))
    {
        return FALSE;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/net/%s/master/ifindex", ifname);
    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        return expected_master_ifindex == 0;
    }

    uint32_t actual_master_ifindex = 0;
    gboolean has_master = fscanf(fp, "%u", &actual_master_ifindex) == 1;
    fclose(fp);

    if (expected_master_ifindex == 0)
    {
        return !has_master;
    }
    return has_master && actual_master_ifindex == expected_master_ifindex;
}

static gboolean os_route_table_matches(uint32_t table_id, uint32_t out_ifindex, uint32_t table_filter,
                                       gboolean has_table_filter, gboolean include_local_table,
                                       uint32_t local_master_ifindex)
{
    if (!has_table_filter)
    {
        return TRUE;
    }
    if (table_id == table_filter)
    {
        return TRUE;
    }
    if (include_local_table && table_id == RT_TABLE_LOCAL)
    {
        return os_if_master_matches(out_ifindex, local_master_ifindex);
    }
    return FALSE;
}

static gboolean os_parse_route(struct nlmsghdr *nlh, GString *buf, uint32_t table_filter, gboolean has_table_filter,
                               gboolean include_local_table, uint32_t local_master_ifindex)
{
    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
    char dst_str[64];
    char gw_str[64] = "-";
    char oif_name[IF_NAMESIZE] = "-";
    char encap_str[128] = "-";
    char newdst_str[128] = "";
    uint32_t priority = 0;
    uint32_t table_id = rtm->rtm_table;
    uint32_t out_ifindex = 0;
    uint16_t encap_type = 0;
    const struct rtattr *encap_attr = NULL;

    if (rtm->rtm_family == AF_INET)
    {
        inet_ntop(AF_INET, "\0\0\0\0", dst_str, sizeof(dst_str));
    }
    else
    {
        inet_ntop(AF_INET6, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", dst_str, sizeof(dst_str));
    }
    if (rtm->rtm_family == AF_MPLS)
    {
        snprintf(dst_str, sizeof(dst_str), "-");
        snprintf(encap_str, sizeof(encap_str), "%s", rtm->rtm_type == RTN_BLACKHOLE ? "drop" : "pop");
    }

    struct rtattr *rta = RTM_RTA(rtm);
    int rta_len = (int)RTM_PAYLOAD(nlh);
    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
    {
        switch (rta->rta_type)
        {
            case RTA_DST:
                if (rtm->rtm_family == AF_MPLS)
                {
                    os_parse_mpls_stack(rta, dst_str, sizeof(dst_str));
                }
                else
                {
                    inet_ntop(rtm->rtm_family, RTA_DATA(rta), dst_str, sizeof(dst_str));
                }
                break;
            case RTA_NEWDST:
                if (rtm->rtm_family == AF_MPLS)
                {
                    os_parse_mpls_stack(rta, newdst_str, sizeof(newdst_str));
                }
                break;
            case RTA_GATEWAY:
                if (rtm->rtm_family == AF_INET || rtm->rtm_family == AF_INET6)
                {
                    inet_ntop(rtm->rtm_family, RTA_DATA(rta), gw_str, sizeof(gw_str));
                }
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
                out_ifindex = idx;
                if (!if_indextoname(idx, oif_name))
                {
                    snprintf(oif_name, sizeof(oif_name), "if%u", idx);
                }
                break;
            }
            case RTA_PRIORITY:
                memcpy(&priority, RTA_DATA(rta), sizeof(priority));
                break;
            case RTA_TABLE:
                memcpy(&table_id, RTA_DATA(rta), sizeof(table_id));
                break;
            case RTA_ENCAP_TYPE:
                memcpy(&encap_type, RTA_DATA(rta), sizeof(encap_type));
                break;
            case RTA_ENCAP:
                encap_attr = rta;
                break;
            default:
                break;
        }
    }

    if (!os_route_table_matches(table_id, out_ifindex, table_filter, has_table_filter, include_local_table,
                                local_master_ifindex))
    {
        return FALSE;
    }

    if (encap_type == LWTUNNEL_ENCAP_MPLS && encap_attr)
    {
        os_parse_mpls_encap(encap_attr, encap_str, sizeof(encap_str));
    }
    if (rtm->rtm_family == AF_MPLS && newdst_str[0] != '\0')
    {
        snprintf(encap_str, sizeof(encap_str), "swap[%.121s]", newdst_str);
    }

    char prefix_str[80];
    if (rtm->rtm_family == AF_MPLS)
    {
        snprintf(prefix_str, sizeof(prefix_str), "%s", dst_str);
    }
    else
    {
        snprintf(prefix_str, sizeof(prefix_str), "%s/%u", dst_str, rtm->rtm_dst_len);
    }
    char table_str[24];
    os_table_format(table_id, table_str, sizeof(table_str));
    g_string_append_printf(buf, "%-7s %-10s %-26s %-20s %-14s %-8s %-7u %s\r\n", table_str, os_type_str(rtm->rtm_type),
                           prefix_str, gw_str, oif_name, os_proto_str(rtm->rtm_protocol), priority, encap_str);
    return TRUE;
}

int fib_os_show(GString *buf, sa_family_t family, uint32_t table_filter, gboolean has_table_filter,
                gboolean include_local_table, uint32_t local_master_ifindex)
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
                           "\r\n%-7s %-10s %-26s %-20s %-14s %-8s %-7s %s\r\n"
                           "------- ---------- -------------------------- "
                           "-------------------- -------------- -------- ------- ------------\r\n",
                           "Table", "Type", "Prefix", "Gateway", "Interface", "Proto", "Metric", "Encap");

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
                if (os_parse_route(nlh, buf, table_filter, has_table_filter, include_local_table, local_master_ifindex))
                {
                    count++;
                }
            }
        }
    }

    g_free(recv_buf);
    close(fd);
    g_string_append_printf(buf, "\r\nTotal %u route(s)\r\n", count);
    return ERRCODE_SUCCESS;
}
