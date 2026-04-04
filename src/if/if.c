/**
 * @file   if.c
 * @brief  接口配置模块，提供不同接口类型的抽象层
 * @author jhb
 * @date   2026/01/22
 */
#include "if.h"

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <ifaddrs.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errcode.h"
#include "if_map.h"
#include "log.h"

// Detect interface type by reading /sys/class/net/<ifname>/type
if_type_t if_detect_type(const char *ifname)
{
    char path[256];
    char type_str[32];
    FILE *fp;

    // Try to read interface type from sysfs
    snprintf(path, sizeof(path), "/sys/class/net/%s/type", ifname);
    fp = fopen(path, "r");
    if (fp == NULL)
    {
        return IF_TYPE_UNKNOWN;
    }

    if (fgets(type_str, sizeof(type_str), fp) == NULL)
    {
        fclose(fp);
        return IF_TYPE_UNKNOWN;
    }
    fclose(fp);

    int type = atoi(type_str);

    // ARPHRD_* constants from linux/if_arp.h
    // 1 = ARPHRD_ETHER (Ethernet)
    // 772 = ARPHRD_LOOPBACK
    // 768 = ARPHRD_TUNNEL

    if (type == 1)
    {
        // Could be eth or veth, check uevent
        snprintf(path, sizeof(path), "/sys/class/net/%s/uevent", ifname);
        fp = fopen(path, "r");
        if (fp != NULL)
        {
            char line[256];
            while (fgets(line, sizeof(line), fp))
            {
                if (strstr(line, "DEVTYPE=veth"))
                {
                    fclose(fp);
                    return IF_TYPE_VETH;
                }
            }
            fclose(fp);
        }
        return IF_TYPE_ETHERNET;
    }
    else if (type == 772)
    {
        return IF_TYPE_LOOPBACK;
    }

    return IF_TYPE_UNKNOWN;
}

// Convert interface type to string
const char *if_type_to_string(if_type_t type)
{
    switch (type)
    {
        case IF_TYPE_ETHERNET:
            return "Ethernet";
        case IF_TYPE_VETH:
            return "Virtual Ethernet";
        case IF_TYPE_LOOPBACK:
            return "Loopback";
        case IF_TYPE_BRIDGE:
            return "Bridge";
        case IF_TYPE_TUN:
            return "TUN/TAP";
        case IF_TYPE_VLAN:
            return "VLAN";
        default:
            return "Unknown";
    }
}

// Get detailed interface information
int if_get_info(const char *ifname, if_info_t *info)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return ERRCODE_FAIL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    memset(info, 0, sizeof(if_info_t));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    strncpy(info->name, ifname, IFNAMSIZ - 1);

    // Get flags
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0)
    {
        info->flags = ifr.ifr_flags;
        info->state = (ifr.ifr_flags & IFF_UP) ? IF_STATE_UP : IF_STATE_DOWN;
    }

    // Get IP address
    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0)
    {
        struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
        inet_ntop(AF_INET, &addr->sin_addr, info->ip_address, INET_ADDRSTRLEN);
    }

    // Get netmask
    if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0)
    {
        struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_netmask;
        inet_ntop(AF_INET, &addr->sin_addr, info->netmask, INET_ADDRSTRLEN);
    }

    // Get MAC address
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0)
    {
        memcpy(info->mac, ifr.ifr_hwaddr.sa_data, 6);
    }

    // Get MTU
    if (ioctl(sock, SIOCGIFMTU, &ifr) == 0)
    {
        info->mtu = ifr.ifr_mtu;
    }

    close(sock);

    // Detect interface type
    info->type = if_detect_type(ifname);

    // TODO: Get RX/TX statistics from /sys/class/net/<ifname>/statistics/

    return ERRCODE_SUCCESS;
}

// Check if interface exists
int if_exists(const char *ifname)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return 0;
    }

    struct ifreq ifr;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    int exists = (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0);
    close(sock);
    return exists;
}

// Set interface state (up/down) - works for any type
int if_set_state(const char *ifname, int up)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return ERRCODE_FAIL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    // Get current flags
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0)
    {
        close(sock);
        return ERRCODE_FAIL;
    }

    // Modify flags
    if (up != 0)
    {
        ifr.ifr_flags |= IFF_UP;
    }
    else
    {
        ifr.ifr_flags &= ~IFF_UP;
    }

    // Set new flags
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0)
    {
        close(sock);
        return ERRCODE_FAIL;
    }

    close(sock);
    return ERRCODE_SUCCESS;
}

// Set MTU
int if_set_mtu(const char *ifname, int mtu)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return ERRCODE_FAIL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_mtu = mtu;

    if (ioctl(sock, SIOCSIFMTU, &ifr) < 0)
    {
        close(sock);
        return ERRCODE_FAIL;
    }

    close(sock);
    return ERRCODE_SUCCESS;
}
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

// Helper to add Netlink attributes
static void if_add_attr(struct nlmsghdr *n, int maxlen, int type, const void *data, int alen)
{
    int len = RTA_LENGTH(alen);
    struct rtattr *rta;

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > (unsigned int)maxlen)
    {
        return;
    }
    rta = (struct rtattr *)(((char *)n) + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = len;
    memcpy(RTA_DATA(rta), data, alen);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
}

static int if_addr_apply(const char *ifname, const net_prefix_t *prefix, int cmd)
{
    if (!ifname || !prefix || !net_prefix_is_set(prefix))
    {
        return ERRCODE_FAIL;
    }

    if (prefix->addr.family != AF_INET && prefix->addr.family != AF_INET6)
    {
        return ERRCODE_FAIL;
    }

    uint8_t max_len = (prefix->addr.family == AF_INET) ? 32u : 128u;
    if (prefix->prefix_len > max_len)
    {
        return ERRCODE_FAIL;
    }

    unsigned int ifidx = if_nametoindex(ifname);
    if (ifidx == 0)
    {
        LOG_ERROR("IF: interface %s not found for addr apply", ifname);
        return ERRCODE_FAIL;
    }

    int sock = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (sock < 0)
    {
        LOG_PERROR("socket(AF_NETLINK) for addr apply");
        return ERRCODE_FAIL;
    }

    struct
    {
        struct nlmsghdr n;
        struct ifaddrmsg ifa;
        char buf[256];
    } req;
    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.n.nlmsg_type = (unsigned short)cmd;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    if (cmd == RTM_NEWADDR)
    {
        req.n.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
    }

    req.ifa.ifa_family = (unsigned char)prefix->addr.family;
    req.ifa.ifa_prefixlen = prefix->prefix_len;
    req.ifa.ifa_scope = RT_SCOPE_UNIVERSE;
    req.ifa.ifa_index = ifidx;

    const void *addr_data = NULL;
    int addr_len = 0;
    if (prefix->addr.family == AF_INET)
    {
        addr_data = &prefix->addr.u.v4.s_addr;
        addr_len = 4;
    }
    else
    {
        addr_data = prefix->addr.u.v6.s6_addr;
        addr_len = 16;
    }

    if_add_attr(&req.n, sizeof(req), IFA_LOCAL, addr_data, addr_len);
    if_add_attr(&req.n, sizeof(req), IFA_ADDRESS, addr_data, addr_len);

    if (send(sock, &req, req.n.nlmsg_len, 0) < 0)
    {
        LOG_PERROR("Netlink send (addr apply)");
        close(sock);
        return ERRCODE_FAIL;
    }

    char ans[4096];
    int len = recv(sock, ans, sizeof(ans), 0);
    close(sock);

    if (len < 0)
    {
        LOG_PERROR("Netlink recv (addr apply)");
        return ERRCODE_FAIL;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)ans;
    if (nlh->nlmsg_type == NLMSG_ERROR)
    {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
        if (err->error < 0)
        {
            int os_err = -err->error;
            if ((cmd == RTM_NEWADDR && os_err == EEXIST) ||
                (cmd == RTM_DELADDR && (os_err == EADDRNOTAVAIL || os_err == ESRCH || os_err == ENOENT)))
            {
                return ERRCODE_SUCCESS;
            }

            char pfx[80];
            net_prefix_to_str(prefix, pfx, sizeof(pfx));
            LOG_ERROR("IF: %s address %s on %s failed: %s", (cmd == RTM_NEWADDR) ? "add" : "del", pfx, ifname,
                      strerror(os_err));
            return ERRCODE_FAIL;
        }
    }

    return ERRCODE_SUCCESS;
}

// Create veth pair using Netlink
static int if_create_veth_netlink(const char *ifname, const char *peer_name)
{
    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0)
    {
        LOG_PERROR("socket(AF_NETLINK)");
        return ERRCODE_FAIL;
    }

    struct
    {
        struct nlmsghdr n;
        struct ifinfomsg i;
        char buf[1024];
    } req;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.n.nlmsg_type = RTM_NEWLINK;
    req.i.ifi_family = AF_UNSPEC;

    if_add_attr(&req.n, sizeof(req), IFLA_IFNAME, ifname, strlen(ifname) + 1);

    // IFLA_LINKINFO
    struct rtattr *linkinfo = (struct rtattr *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
    if_add_attr(&req.n, sizeof(req), IFLA_LINKINFO, NULL, 0);

    if_add_attr(&req.n, sizeof(req), IFLA_INFO_KIND, "veth", 5);

    // IFLA_INFO_DATA
    struct rtattr *infodata = (struct rtattr *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
    if_add_attr(&req.n, sizeof(req), IFLA_INFO_DATA, NULL, 0);

    // VETH_INFO_PEER
    struct rtattr *peerinfo = (struct rtattr *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
    if_add_attr(&req.n, sizeof(req), 1, NULL, 0); // VETH_INFO_PEER is 1

    struct ifinfomsg peer_ifi;
    memset(&peer_ifi, 0, sizeof(peer_ifi));
    peer_ifi.ifi_family = AF_UNSPEC;

    // Append peer ifinfomsg to the VETH_INFO_PEER attribute
    int peer_ifi_len = sizeof(struct ifinfomsg);
    memcpy(RTA_DATA(peerinfo), &peer_ifi, peer_ifi_len);
    req.n.nlmsg_len += NLMSG_ALIGN(peer_ifi_len);
    peerinfo->rta_len += NLMSG_ALIGN(peer_ifi_len);

    // Add peer name as IFLA_IFNAME to the peer info
    if_add_attr(&req.n, sizeof(req), IFLA_IFNAME, peer_name, strlen(peer_name) + 1);
    peerinfo->rta_len += RTA_ALIGN(RTA_LENGTH(strlen(peer_name) + 1));

    // Fix up lengths
    infodata->rta_len = (char *)(((char *)&req.n) + req.n.nlmsg_len) - (char *)infodata;
    linkinfo->rta_len = (char *)(((char *)&req.n) + req.n.nlmsg_len) - (char *)linkinfo;

    if (send(sock, &req, req.n.nlmsg_len, 0) < 0)
    {
        LOG_PERROR("Netlink send");
        close(sock);
        return ERRCODE_FAIL;
    }

    // Wait for ACK
    char ans[4096];
    int len = recv(sock, ans, sizeof(ans), 0);
    if (len < 0)
    {
        LOG_PERROR("Netlink recv");
        close(sock);
        return ERRCODE_FAIL;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)ans;
    if (nlh->nlmsg_type == NLMSG_ERROR)
    {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
        if (err->error < 0)
        {
            LOG_ERROR("Netlink error: %s", strerror(-err->error));
            close(sock);
            return ERRCODE_FAIL;
        }
    }

    close(sock);
    return ERRCODE_SUCCESS;
}

/* 使用 Netlink 创建 dummy 接口 */
int if_create_dummy(const char *ifname)
{
    if (!ifname)
    {
        return ERRCODE_FAIL;
    }

    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0)
    {
        LOG_PERROR("socket(AF_NETLINK)");
        return ERRCODE_FAIL;
    }

    struct
    {
        struct nlmsghdr n;
        struct ifinfomsg i;
        char buf[256];
    } req;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.n.nlmsg_type = RTM_NEWLINK;
    req.i.ifi_family = AF_UNSPEC;

    if_add_attr(&req.n, sizeof(req), IFLA_IFNAME, ifname, strlen(ifname) + 1);

    /* IFLA_LINKINFO → IFLA_INFO_KIND = "dummy" */
    struct rtattr *linkinfo = (struct rtattr *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
    if_add_attr(&req.n, sizeof(req), IFLA_LINKINFO, NULL, 0);
    if_add_attr(&req.n, sizeof(req), IFLA_INFO_KIND, "dummy", 6);
    linkinfo->rta_len = (uint16_t)((char *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len)) - (char *)linkinfo);

    if (send(sock, &req, req.n.nlmsg_len, 0) < 0)
    {
        LOG_PERROR("Netlink send (create dummy)");
        close(sock);
        return ERRCODE_FAIL;
    }

    char ans[4096];
    int len = recv(sock, ans, sizeof(ans), 0);
    close(sock);

    if (len < 0)
    {
        LOG_PERROR("Netlink recv (create dummy)");
        return ERRCODE_FAIL;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)ans;
    if (nlh->nlmsg_type == NLMSG_ERROR)
    {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
        if (err->error < 0 && err->error != -EEXIST)
        {
            LOG_ERROR("Netlink error creating dummy %s: %s", ifname, strerror(-err->error));
            return ERRCODE_FAIL;
        }
    }

    LOG_INFO("Created dummy interface %s", ifname);
    return ERRCODE_SUCCESS;
}

/* 使用 Netlink RTM_DELLINK 删除接口 */
int if_delete_interface(const char *ifname)
{
    if (!ifname)
    {
        return ERRCODE_FAIL;
    }

    unsigned int ifidx = if_nametoindex(ifname);
    if (ifidx == 0)
    {
        LOG_WARN("IF: interface %s not found for deletion", ifname);
        return ERRCODE_SUCCESS;
    }

    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0)
    {
        LOG_PERROR("socket(AF_NETLINK)");
        return ERRCODE_FAIL;
    }

    struct
    {
        struct nlmsghdr n;
        struct ifinfomsg i;
    } req;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.n.nlmsg_type = RTM_DELLINK;
    req.i.ifi_family = AF_UNSPEC;
    req.i.ifi_index = (int)ifidx;

    if (send(sock, &req, req.n.nlmsg_len, 0) < 0)
    {
        LOG_PERROR("Netlink send (delete iface)");
        close(sock);
        return ERRCODE_FAIL;
    }

    char ans[4096];
    int len = recv(sock, ans, sizeof(ans), 0);
    close(sock);

    if (len < 0)
    {
        LOG_PERROR("Netlink recv (delete iface)");
        return ERRCODE_FAIL;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)ans;
    if (nlh->nlmsg_type == NLMSG_ERROR)
    {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
        if (err->error < 0)
        {
            LOG_ERROR("Netlink error deleting %s: %s", ifname, strerror(-err->error));
            return ERRCODE_FAIL;
        }
    }

    LOG_INFO("Deleted interface %s", ifname);
    return ERRCODE_SUCCESS;
}

// Ensure interface exists (create if not)
int if_ensure_exists(const char *ifname)
{
    if (if_exists(ifname))
    {
        return ERRCODE_SUCCESS;
    }

    LOG_INFO("Interface %s not found, attempting to create veth pair via Netlink...", ifname);

    char peer_name[IFNAMSIZ];
    snprintf(peer_name, sizeof(peer_name), "%s-peer", ifname);

    if (if_create_veth_netlink(ifname, peer_name) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Failed to create veth pair for %s (check CAP_NET_ADMIN)", ifname);
        return ERRCODE_FAIL;
    }

    // Bring both up
    if_set_state(ifname, 1);
    if_set_state(peer_name, 1);

    LOG_INFO("Successfully created %s and its peer", ifname);
    return ERRCODE_SUCCESS;
}

int if_addr_add_prefix(const char *ifname, const net_prefix_t *prefix)
{
    return if_addr_apply(ifname, prefix, RTM_NEWADDR);
}

int if_addr_del_prefix(const char *ifname, const net_prefix_t *prefix)
{
    return if_addr_apply(ifname, prefix, RTM_DELADDR);
}
