/**
 * @file   if_link_monitor.c
 * @brief  Netlink 链路状态监听线程实现
 *
 * 监听 RTMGRP_LINK 组播事件。监听线程仅负责解析并投递事件到 IF work 线程，
 * 实际的接口恢复、路由撤销与事件发布由 IF work 线程串行处理。
 *
 * @author jhb
 * @date   2026/04/20
 */
#include "if_link_monitor.h"

#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errcode.h"
#include "if_event.h"
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

/* ============================================================================
 * 监听线程主循环
 * ============================================================================ */

static void *link_monitor_thread(void *arg)
{
    (void)arg;

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
            if (nlh->nlmsg_type != RTM_NEWLINK && nlh->nlmsg_type != RTM_DELLINK)
            {
                continue;
            }

            char ifname[IFNAMSIZ];
            uint32_t ifindex = 0u;
            uint8_t link_up_known = 0u;
            uint8_t link_up = 0u;
            if (parse_link_msg(nlh, ifname, sizeof(ifname), &ifindex, &link_up_known, &link_up) != 0)
            {
                continue;
            }

            if (ifname[0] == '\0')
            {
                continue;
            }

            if (if_worker_post_link_event((uint16_t)nlh->nlmsg_type, ifname, ifindex, link_up_known, link_up) !=
                ERRCODE_SUCCESS)
            {
                LOG_WARN("IF-MONITOR: failed to enqueue link event type=%u if=%s ifindex=%u",
                         (unsigned int)nlh->nlmsg_type, ifname, ifindex);
            }
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
    sa.nl_groups = RTMGRP_LINK;

    if (bind(g_monitor_nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        LOG_ERROR("IF-MONITOR: bind() failed: %s", strerror(errno));
        close(g_monitor_nl_fd);
        g_monitor_nl_fd = -1;
        return -1;
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
        (void)write(g_monitor_pipe[1], &c, 1);
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
