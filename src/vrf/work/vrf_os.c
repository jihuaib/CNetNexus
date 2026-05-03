/**
 * @file   vrf_os.c
 * @brief  VRF L3VRF netlink 下发实现
 * @author jhb
 * @date   2026/05/02
 */
#include "vrf_os.h"

#include <errno.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"

#define VRF_OS_NL_BUFSIZE 1024

/* IFLA_VRF_TABLE 由内核 vrf 模块定义；某些发行版头未引出，作为 fallback 显式声明。 */
#ifndef IFLA_VRF_TABLE
#    define IFLA_VRF_TABLE 1
#endif

static void nl_add_attr(struct nlmsghdr *nlh, size_t maxlen, int type, const void *data, int datalen)
{
    int len = RTA_LENGTH(datalen);
    if (NLMSG_ALIGN(nlh->nlmsg_len) + (size_t)RTA_ALIGN(len) > maxlen)
    {
        LOG_WARN("vrf_os: netlink attr overflow, type=%d", type);
        return;
    }
    struct rtattr *rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)len;
    if (datalen > 0 && data)
    {
        memcpy(RTA_DATA(rta), data, (size_t)datalen);
    }
    nlh->nlmsg_len = (unsigned int)(NLMSG_ALIGN(nlh->nlmsg_len) + (size_t)RTA_ALIGN(len));
}

static struct rtattr *nl_nest_start(struct nlmsghdr *nlh, size_t maxlen, int type)
{
    if (NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(sizeof(struct rtattr)) > maxlen)
    {
        return NULL;
    }
    struct rtattr *nest = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    nest->rta_type = (unsigned short)(type | NLA_F_NESTED);
    nest->rta_len = (unsigned short)RTA_LENGTH(0);
    nlh->nlmsg_len = (unsigned int)(NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(sizeof(struct rtattr)));
    return nest;
}

static void nl_nest_end(struct nlmsghdr *nlh, struct rtattr *nest)
{
    nest->rta_len = (unsigned short)((char *)nlh + nlh->nlmsg_len - (char *)nest);
}

static int nl_exchange(struct nlmsghdr *nlh, int allow_eexist, int allow_enodev)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        LOG_ERROR("vrf_os: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;

    if (sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&nladdr, sizeof(nladdr)) < 0)
    {
        LOG_ERROR("vrf_os: sendto() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    char ack_buf[VRF_OS_NL_BUFSIZE];
    ssize_t n = recv(fd, ack_buf, sizeof(ack_buf), 0);
    close(fd);
    if (n < 0)
    {
        LOG_ERROR("vrf_os: recv() failed: %s", strerror(errno));
        return -1;
    }

    struct nlmsghdr *ack = (struct nlmsghdr *)(void *)ack_buf;
    if (ack->nlmsg_type == NLMSG_ERROR)
    {
        const struct nlmsgerr *err = (const struct nlmsgerr *)NLMSG_DATA(ack);
        if (err->error == 0)
        {
            return 0;
        }
        if (allow_eexist && err->error == -EEXIST)
        {
            return 0;
        }
        if (allow_enodev && (err->error == -ENODEV || err->error == -ENOENT))
        {
            return 0;
        }
        LOG_WARN("vrf_os: netlink error %d: %s", -err->error, strerror(-err->error));
        return -1;
    }
    return 0;
}

static int set_link_state(const char *name, int up)
{
    unsigned int idx = if_nametoindex(name);
    if (idx == 0)
    {
        return -1;
    }

    char buf[VRF_OS_NL_BUFSIZE];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = (struct nlmsghdr *)(void *)buf;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nlh->nlmsg_type = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = (int)idx;
    ifi->ifi_change = IFF_UP;
    ifi->ifi_flags = up ? IFF_UP : 0u;

    return nl_exchange(nlh, 0, 0);
}

int vrf_os_install(const char *name, uint32_t table_id)
{
    if (!name || name[0] == '\0' || table_id == 0)
    {
        return -1;
    }

    char buf[VRF_OS_NL_BUFSIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)(void *)buf;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nlh->nlmsg_type = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;

    nl_add_attr(nlh, sizeof(buf), IFLA_IFNAME, name, (int)strlen(name) + 1);

    struct rtattr *linkinfo = nl_nest_start(nlh, sizeof(buf), IFLA_LINKINFO);
    if (!linkinfo)
    {
        return -1;
    }
    static const char kind[] = "vrf";
    nl_add_attr(nlh, sizeof(buf), IFLA_INFO_KIND, kind, (int)sizeof(kind));

    struct rtattr *info_data = nl_nest_start(nlh, sizeof(buf), IFLA_INFO_DATA);
    if (!info_data)
    {
        return -1;
    }
    nl_add_attr(nlh, sizeof(buf), IFLA_VRF_TABLE, &table_id, (int)sizeof(table_id));
    nl_nest_end(nlh, info_data);
    nl_nest_end(nlh, linkinfo);

    int rc = nl_exchange(nlh, /*allow_eexist=*/1, /*allow_enodev=*/0);
    if (rc != 0)
    {
        LOG_ERROR("vrf_os: failed to add l3vrf %s table=%u", name, table_id);
        return -1;
    }

    /* 创建后立即 set up；EEXIST 路径下也需要确保 UP */
    if (set_link_state(name, 1) != 0)
    {
        LOG_WARN("vrf_os: %s set up failed", name);
    }

    LOG_INFO("vrf_os: l3vrf installed name=%s table=%u", name, table_id);
    return 0;
}

/* ============================================================================
 * dump：枚举 type=vrf 的内核设备
 * ========================================================================== */

static const char *iface_oper_str(uint8_t state)
{
    switch (state)
    {
        case 6: /* IF_OPER_UP */
            return "UP";
        case 5: /* IF_OPER_DORMANT */
            return "DORMANT";
        case 2: /* IF_OPER_DOWN */
            return "DOWN";
        default:
            return "UNKNOWN";
    }
}

static int parse_vrf_link(const struct nlmsghdr *nlh, GString *buf)
{
    const struct ifinfomsg *ifi = (const struct ifinfomsg *)NLMSG_DATA(nlh);
    int rta_len = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi)));
    if (rta_len < 0)
    {
        return 0;
    }

    const char *ifname = NULL;
    uint8_t oper = 0;
    int is_vrf = 0;
    uint32_t table_id = 0;

    for (const struct rtattr *rta = IFLA_RTA(ifi); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
    {
        if (rta->rta_type == IFLA_IFNAME)
        {
            ifname = (const char *)RTA_DATA(rta);
        }
        else if (rta->rta_type == IFLA_OPERSTATE && RTA_PAYLOAD(rta) >= 1)
        {
            oper = *(const uint8_t *)RTA_DATA(rta);
        }
        else if (rta->rta_type == IFLA_LINKINFO)
        {
            int li_len = (int)RTA_PAYLOAD(rta);
            for (const struct rtattr *li = (const struct rtattr *)RTA_DATA(rta); RTA_OK(li, li_len);
                 li = RTA_NEXT(li, li_len))
            {
                if (li->rta_type == IFLA_INFO_KIND)
                {
                    const char *kind = (const char *)RTA_DATA(li);
                    if (strncmp(kind, "vrf", 3) == 0)
                    {
                        is_vrf = 1;
                    }
                }
                else if (li->rta_type == IFLA_INFO_DATA)
                {
                    int id_len = (int)RTA_PAYLOAD(li);
                    for (const struct rtattr *idr = (const struct rtattr *)RTA_DATA(li); RTA_OK(idr, id_len);
                         idr = RTA_NEXT(idr, id_len))
                    {
                        if (idr->rta_type == IFLA_VRF_TABLE && RTA_PAYLOAD(idr) >= sizeof(uint32_t))
                        {
                            memcpy(&table_id, RTA_DATA(idr), sizeof(table_id));
                        }
                    }
                }
            }
        }
    }

    if (!is_vrf || !ifname)
    {
        return 0;
    }

    g_string_append_printf(buf, "  %-20s  %-10u  %-8d  %s\r\n", ifname, table_id, ifi->ifi_index, iface_oper_str(oper));
    return 1;
}

int vrf_os_show(GString *buf)
{
    if (!buf)
    {
        return -1;
    }

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        LOG_ERROR("vrf_os: dump socket() failed: %s", strerror(errno));
        return -1;
    }

    struct
    {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifi));
    req.nlh.nlmsg_type = RTM_GETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.ifi.ifi_family = AF_UNSPEC;

    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;

    if (sendto(fd, &req, req.nlh.nlmsg_len, 0, (struct sockaddr *)&nladdr, sizeof(nladdr)) < 0)
    {
        LOG_ERROR("vrf_os: dump sendto() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    g_string_append(buf, "OS L3VRF Devices:\r\n");
    g_string_append_printf(buf, "  %-20s  %-10s  %-8s  %s\r\n", "Name", "Table-ID", "IfIndex", "OperState");
    g_string_append(buf, "  --------------------  ----------  --------  ---------\r\n");

    char rbuf[8192];
    int done = 0;
    int total = 0;
    while (!done)
    {
        ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
        if (n < 0)
        {
            LOG_ERROR("vrf_os: dump recv() failed: %s", strerror(errno));
            close(fd);
            return -1;
        }
        if (n == 0)
        {
            break;
        }
        for (struct nlmsghdr *nlh = (struct nlmsghdr *)(void *)rbuf; NLMSG_OK(nlh, (size_t)n); nlh = NLMSG_NEXT(nlh, n))
        {
            if (nlh->nlmsg_type == NLMSG_DONE)
            {
                done = 1;
                break;
            }
            if (nlh->nlmsg_type == NLMSG_ERROR)
            {
                done = 1;
                break;
            }
            if (nlh->nlmsg_type == RTM_NEWLINK)
            {
                total += parse_vrf_link(nlh, buf);
            }
        }
    }
    close(fd);

    if (total == 0)
    {
        g_string_append(buf, "  (none)\r\n");
    }
    return 0;
}

int vrf_os_remove(const char *name)
{
    if (!name || name[0] == '\0')
    {
        return -1;
    }

    unsigned int idx = if_nametoindex(name);
    if (idx == 0)
    {
        return 0; /* 不存在视为成功 */
    }

    char buf[VRF_OS_NL_BUFSIZE];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = (struct nlmsghdr *)(void *)buf;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nlh->nlmsg_type = RTM_DELLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = (int)idx;

    int rc = nl_exchange(nlh, /*allow_eexist=*/0, /*allow_enodev=*/1);
    if (rc != 0)
    {
        LOG_ERROR("vrf_os: failed to delete l3vrf %s", name);
        return -1;
    }
    LOG_INFO("vrf_os: l3vrf removed name=%s", name);
    return 0;
}
