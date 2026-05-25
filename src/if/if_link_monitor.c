/**
 * @file   if_link_monitor.c
 * @brief  Netlink 接口状态监听线程实现
 *
 * 监听 RTMGRP_LINK / RTMGRP_IPV6_IFADDR 组播事件。监听线程仅负责解析并投递
 * link / IPv6 link-local 地址事件到 IF work 线程，实际的运行态更新、路由撤销
 * 与事件发布由 IF work 线程串行处理。
 *
 * @author jhb
 * @date   2026/04/20
 */
#include "if_link_monitor.h"

#include <errno.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errcode.h"
#include "if.h"
#include "if_main.h"
#include "log.h"
#include "work/if_cfg_apply.h"
#include "work/if_map.h"
#include "work/if_pub.h"
#include "work/if_worker.h"

#define LINK_MON_BUF_SIZE 8192u

/* ============================================================================
 * 模块静态变量
 * ============================================================================ */

static pthread_t g_monitor_thread;
static int g_monitor_pipe[2] = {-1, -1}; /**< 写端用于唤醒 poll 以通知退出 */
static int g_monitor_nl_fd = -1;         /**< Netlink 套接字 fd */
static volatile int g_monitor_running = 0;
static volatile int g_monitor_started = 0; /**< pthread_create 是否成功 */

/* ============================================================================
 * Netlink 消息解析
 * ============================================================================ */

/**
 * @brief 从 RTM_NEWLINK/RTM_DELLINK 消息中提取接口名、ifindex 和链路状态
 * @return 0 成功，-1 失败
 */
static int parse_link_msg(struct nlmsghdr *nlh, char *ifname, size_t ifname_size, uint32_t *ifindex,
                          uint8_t *link_up_known, uint8_t *link_up)
{
    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    *ifindex = (uint32_t)ifi->ifi_index;
    ifname[0] = '\0';
    *link_up_known = 0u;
    *link_up = 0u;

    int carrier = -1;
    int operstate = -1;
    int admin_up = (ifi->ifi_flags & IFF_UP) ? 1 : 0;

    struct rtattr *rta = IFLA_RTA(ifi);
    int rta_len = (int)IFLA_PAYLOAD(nlh);

    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
    {
        if (rta->rta_type == IFLA_IFNAME)
        {
            snprintf(ifname, ifname_size, "%s", (const char *)RTA_DATA(rta));
            continue;
        }

        if (rta->rta_type == IFLA_CARRIER && RTA_PAYLOAD(rta) >= sizeof(uint8_t))
        {
            carrier = (int)(*(const uint8_t *)RTA_DATA(rta));
            continue;
        }

        if (rta->rta_type == IFLA_OPERSTATE && RTA_PAYLOAD(rta) >= sizeof(uint8_t))
        {
            operstate = (int)(*(const uint8_t *)RTA_DATA(rta));
            continue;
        }
    }

    if (ifname[0] == '\0')
    {
        return -1;
    }

    if (nlh->nlmsg_type == RTM_DELLINK)
    {
        *link_up_known = 1u;
        *link_up = 0u;
        return 0;
    }

    int resolved = -1;
    if (carrier >= 0)
    {
        resolved = (carrier == 1) ? 1 : 0;
    }
    else if (operstate >= 0)
    {
#ifdef IF_OPER_UP
        resolved = (operstate == IF_OPER_UP) ? 1 : 0;
#else
        resolved = (operstate == 6) ? 1 : 0;
#endif
    }
#ifdef IFF_LOWER_UP
    else if ((ifi->ifi_flags & IFF_LOWER_UP) != 0)
    {
        resolved = 1;
    }
#elif defined(IFF_RUNNING)
    else if ((ifi->ifi_flags & IFF_RUNNING) != 0)
    {
        resolved = 1;
    }
#endif

    if (!admin_up)
    {
        resolved = 0;
    }

    if (resolved >= 0)
    {
        *link_up_known = 1u;
        *link_up = (resolved != 0) ? 1u : 0u;
    }

    return 0;
}

/**
 * @brief 从 RTM_NEWADDR/RTM_DELADDR 消息中提取 IPv6 link-local 地址信息
 * @return 0 成功，-1 失败或不是目标地址
 */
static int parse_addr_msg(struct nlmsghdr *nlh, char *ifname, size_t ifname_size, uint32_t *ifindex,
                          uint32_t *addr_flags, net_prefix_t *prefix)
{
    if (!nlh || !ifname || ifname_size == 0 || !ifindex || !addr_flags || !prefix)
    {
        return -1;
    }

    struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
    if (!ifa || ifa->ifa_family != AF_INET6)
    {
        return -1;
    }

    ifname[0] = '\0';
    *ifindex = ifa->ifa_index;
    *addr_flags = 0u;
    memset(prefix, 0, sizeof(*prefix));

    const struct in6_addr *addr_v6 = NULL;
    struct rtattr *rta = IFA_RTA(ifa);
    int rta_len = (int)IFA_PAYLOAD(nlh);

    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
    {
        if (rta->rta_type == IFA_LABEL)
        {
            snprintf(ifname, ifname_size, "%s", (const char *)RTA_DATA(rta));
            continue;
        }

        if ((rta->rta_type == IFA_LOCAL || rta->rta_type == IFA_ADDRESS) && RTA_PAYLOAD(rta) >= sizeof(struct in6_addr))
        {
            addr_v6 = (const struct in6_addr *)RTA_DATA(rta);
        }
    }

    if (ifname[0] == '\0' && *ifindex != 0u)
    {
        if_indextoname((unsigned int)(*ifindex), ifname);
    }

    if (ifname[0] == '\0' || !addr_v6)
    {
        return -1;
    }

    prefix->addr.family = AF_INET6;
    memcpy(&prefix->addr.u.v6, addr_v6, sizeof(prefix->addr.u.v6));
    prefix->prefix_len = ifa->ifa_prefixlen;

    if (IN6_IS_ADDR_LINKLOCAL(addr_v6) || ifa->ifa_scope == RT_SCOPE_LINK)
    {
        *addr_flags |= IF_ADDR_FLAG_LINK_LOCAL;
    }

    return ((*addr_flags & IF_ADDR_FLAG_LINK_LOCAL) != 0u) ? 0 : -1;
}

static void post_addr_event(struct nlmsghdr *nlh)
{
    char ifname[IFNAMSIZ];
    uint32_t ifindex = 0u;
    uint32_t addr_flags = 0u;
    net_prefix_t prefix;

    if (parse_addr_msg(nlh, ifname, sizeof(ifname), &ifindex, &addr_flags, &prefix) != 0)
    {
        return;
    }

    if (if_worker_post_addr_event((uint16_t)nlh->nlmsg_type, ifname, ifindex, addr_flags, &prefix) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF-MONITOR: failed to enqueue addr event type=%u if=%s ifindex=%u", (unsigned int)nlh->nlmsg_type,
                 ifname, ifindex);
    }
}

static void handle_netlink_msg(struct nlmsghdr *nlh)
{
    if (!nlh)
    {
        return;
    }

    if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK)
    {
        char ifname[IFNAMSIZ];
        uint32_t ifindex = 0u;
        uint8_t link_up_known = 0u;
        uint8_t link_up = 0u;
        if (parse_link_msg(nlh, ifname, sizeof(ifname), &ifindex, &link_up_known, &link_up) != 0)
        {
            return;
        }

        if (ifname[0] == '\0')
        {
            return;
        }

        if (if_worker_post_link_event((uint16_t)nlh->nlmsg_type, ifname, ifindex, link_up_known, link_up) !=
            ERRCODE_SUCCESS)
        {
            LOG_WARN("IF-MONITOR: failed to enqueue link event type=%u if=%s ifindex=%u", (unsigned int)nlh->nlmsg_type,
                     ifname, ifindex);
        }
        return;
    }

    if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR)
    {
        post_addr_event(nlh);
    }
}

static int dump_initial_ipv6_addrs(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        LOG_WARN("IF-MONITOR: startup IPv6 addr dump socket() failed: %s", strerror(errno));
        return -1;
    }

    struct
    {
        struct nlmsghdr nlh;
        struct ifaddrmsg ifa;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.nlh.nlmsg_type = RTM_GETADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 1;
    req.ifa.ifa_family = AF_INET6;

    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;

    if (sendto(fd, &req, req.nlh.nlmsg_len, 0, (struct sockaddr *)&nladdr, sizeof(nladdr)) < 0)
    {
        LOG_WARN("IF-MONITOR: startup IPv6 addr dump sendto() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    uint32_t count = 0u;
    gboolean done = FALSE;
    char buf[LINK_MON_BUF_SIZE];

    while (!done)
    {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        if (len < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_WARN("IF-MONITOR: startup IPv6 addr dump recv() failed: %s", strerror(errno));
            close(fd);
            return -1;
        }

        struct nlmsghdr *nlh = (struct nlmsghdr *)(void *)buf;
        for (; NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len))
        {
            if (nlh->nlmsg_type == NLMSG_DONE)
            {
                done = TRUE;
                break;
            }

            if (nlh->nlmsg_type == NLMSG_ERROR)
            {
                const struct nlmsgerr *err = (const struct nlmsgerr *)NLMSG_DATA(nlh);
                if (err && err->error != 0)
                {
                    LOG_WARN("IF-MONITOR: startup IPv6 addr dump error %d: %s", -err->error, strerror(-err->error));
                    close(fd);
                    return -1;
                }
                done = TRUE;
                break;
            }

            if (nlh->nlmsg_type != RTM_NEWADDR)
            {
                continue;
            }

            char ifname[IFNAMSIZ];
            uint32_t ifindex = 0u;
            uint32_t addr_flags = 0u;
            net_prefix_t prefix;
            if (parse_addr_msg(nlh, ifname, sizeof(ifname), &ifindex, &addr_flags, &prefix) != 0)
            {
                continue;
            }

            if (if_worker_post_addr_event(RTM_NEWADDR, ifname, ifindex, addr_flags, &prefix) == ERRCODE_SUCCESS)
            {
                count++;
            }
            else
            {
                LOG_WARN("IF-MONITOR: failed to enqueue startup addr dump event if=%s ifindex=%u", ifname, ifindex);
            }
        }
    }

    close(fd);
    LOG_INFO("IF-MONITOR: startup IPv6 link-local dump queued %u event(s)", count);
    return 0;
}

/* ============================================================================
 * 监听线程主循环
 * ============================================================================ */

static void *link_monitor_thread(void *arg)
{
    (void)arg;

    log_set_tag("if");

    char buf[LINK_MON_BUF_SIZE];
    struct pollfd fds[2];
    fds[0].fd = g_monitor_nl_fd;
    fds[0].events = POLLIN;
    fds[1].fd = g_monitor_pipe[0];
    fds[1].events = POLLIN;

    LOG_INFO("IF-MONITOR: thread started, netlink fd=%d", g_monitor_nl_fd);

    while (g_monitor_running)
    {
        int ret = poll(fds, 2, -1);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_ERROR("IF-MONITOR: poll() failed: %s", strerror(errno));
            break;
        }

        if (fds[1].revents & POLLIN)
        {
            break;
        }

        if (!(fds[0].revents & POLLIN))
        {
            continue;
        }

        ssize_t len = recv(g_monitor_nl_fd, buf, sizeof(buf), 0);
        if (len <= 0)
        {
            if (len < 0 && errno == EINTR)
            {
                continue;
            }
            if (len < 0)
            {
                LOG_ERROR("IF-MONITOR: recv() failed: %s", strerror(errno));
            }
            continue;
        }

        struct nlmsghdr *nlh = (struct nlmsghdr *)(void *)buf;
        for (; NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len))
        {
            handle_netlink_msg(nlh);
        }
    }

    LOG_INFO("IF-MONITOR: thread exiting");
    return NULL;
}

/* ============================================================================
 * 公共 API
 * ============================================================================ */

int if_link_monitor_start(void)
{
    if (!g_if_work_local)
    {
        LOG_ERROR("IF-MONITOR: g_if_work_local not initialized");
        return -1;
    }

    if (!if_worker_is_running())
    {
        LOG_ERROR("IF-MONITOR: IF worker is not running");
        return -1;
    }

    g_monitor_nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (g_monitor_nl_fd < 0)
    {
        LOG_ERROR("IF-MONITOR: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV6_IFADDR;

    if (bind(g_monitor_nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        LOG_ERROR("IF-MONITOR: bind() failed: %s", strerror(errno));
        close(g_monitor_nl_fd);
        g_monitor_nl_fd = -1;
        return -1;
    }

    if (dump_initial_ipv6_addrs() != 0)
    {
        LOG_WARN("IF-MONITOR: startup IPv6 link-local dump failed");
    }

    if (pipe(g_monitor_pipe) < 0)
    {
        LOG_ERROR("IF-MONITOR: pipe() failed: %s", strerror(errno));
        close(g_monitor_nl_fd);
        g_monitor_nl_fd = -1;
        return -1;
    }

    g_monitor_running = 1;

    if (pthread_create(&g_monitor_thread, NULL, link_monitor_thread, NULL) != 0)
    {
        LOG_ERROR("IF-MONITOR: pthread_create() failed: %s", strerror(errno));
        g_monitor_running = 0;
        close(g_monitor_nl_fd);
        close(g_monitor_pipe[0]);
        close(g_monitor_pipe[1]);
        g_monitor_nl_fd = -1;
        g_monitor_pipe[0] = g_monitor_pipe[1] = -1;
        return -1;
    }

    g_monitor_started = 1;
    LOG_INFO("IF-MONITOR: link monitor started");
    return 0;
}

void if_link_monitor_stop(void)
{
    if (!g_monitor_started)
    {
        return;
    }

    g_monitor_running = 0;

    if (g_monitor_pipe[1] >= 0)
    {
        char c = 'x';
        ssize_t wret = write(g_monitor_pipe[1], &c, 1);
        (void)wret;
    }

    (void)pthread_join(g_monitor_thread, NULL);
    g_monitor_started = 0;

    if (g_monitor_nl_fd >= 0)
    {
        close(g_monitor_nl_fd);
        g_monitor_nl_fd = -1;
    }
    if (g_monitor_pipe[0] >= 0)
    {
        close(g_monitor_pipe[0]);
        g_monitor_pipe[0] = -1;
    }
    if (g_monitor_pipe[1] >= 0)
    {
        close(g_monitor_pipe[1]);
        g_monitor_pipe[1] = -1;
    }

    LOG_INFO("IF-MONITOR: link monitor stopped");
}
