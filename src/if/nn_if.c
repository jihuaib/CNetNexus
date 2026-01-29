// Interface configuration module for NetNexus
// Provides abstraction layer for different interface types

#include "nn_if.h"

#include <arpa/inet.h>
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

#include "nn_errcode.h"
#include "nn_if_map.h"

// Current interface context (for interface mode)
char g_current_interface[IFNAMSIZ] = {0};

// Detect interface type by reading /sys/class/net/<ifname>/type
nn_if_type_t nn_if_detect_type(const char *ifname)
{
    char path[256];
    char type_str[32];
    FILE *fp;

    // Try to read interface type from sysfs
    snprintf(path, sizeof(path), "/sys/class/net/%s/type", ifname);
    fp = fopen(path, "r");
    if (fp == NULL)
    {
        return NN_IF_TYPE_UNKNOWN;
    }

    if (fgets(type_str, sizeof(type_str), fp) == NULL)
    {
        fclose(fp);
        return NN_IF_TYPE_UNKNOWN;
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
                    return NN_IF_TYPE_VETH;
                }
            }
            fclose(fp);
        }
        return NN_IF_TYPE_ETHERNET;
    }
    else if (type == 772)
    {
        return NN_IF_TYPE_LOOPBACK;
    }

    return NN_IF_TYPE_UNKNOWN;
}

// Convert interface type to string
const char *nn_if_type_to_string(nn_if_type_t type)
{
    switch (type)
    {
        case NN_IF_TYPE_ETHERNET:
            return "Ethernet";
        case NN_IF_TYPE_VETH:
            return "Virtual Ethernet";
        case NN_IF_TYPE_LOOPBACK:
            return "Loopback";
        case NN_IF_TYPE_BRIDGE:
            return "Bridge";
        case NN_IF_TYPE_TUN:
            return "TUN/TAP";
        case NN_IF_TYPE_VLAN:
            return "VLAN";
        default:
            return "Unknown";
    }
}

// List all available interfaces
int nn_if_list(nn_if_info_t **interfaces, int *count)
{
    struct ifaddrs *ifaddr, *ifa;
    int n = 0;

    if (getifaddrs(&ifaddr) == -1)
    {
        return NN_ERRCODE_FAIL;
    }

    // Count interfaces
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
        {
            continue;
        }
        if (ifa->ifa_addr->sa_family == AF_PACKET)
        {
            n++;
        }
    }

    *interfaces = g_malloc0(sizeof(nn_if_info_t) * n);
    if (*interfaces == NULL)
    {
        freeifaddrs(ifaddr);
        return NN_ERRCODE_FAIL;
    }

    // Fill interface information
    int idx = 0;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
        {
            continue;
        }
        if (ifa->ifa_addr->sa_family == AF_PACKET)
        {
            nn_if_get_info(ifa->ifa_name, &(*interfaces)[idx]);
            idx++;
        }
    }

    *count = n;
    freeifaddrs(ifaddr);
    return NN_ERRCODE_SUCCESS;
}

// Get detailed interface information
int nn_if_get_info(const char *ifname, nn_if_info_t *info)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return NN_ERRCODE_FAIL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    memset(info, 0, sizeof(nn_if_info_t));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    strncpy(info->name, ifname, IFNAMSIZ - 1);

    // Get flags
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0)
    {
        info->flags = ifr.ifr_flags;
        info->state = (ifr.ifr_flags & IFF_UP) ? NN_IF_STATE_UP : NN_IF_STATE_DOWN;
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
    info->type = nn_if_detect_type(ifname);

    // TODO: Get RX/TX statistics from /sys/class/net/<ifname>/statistics/

    return NN_ERRCODE_SUCCESS;
}

// Check if interface exists
int nn_if_exists(const char *ifname)
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

// Set IP address (works for any interface type)
int nn_if_set_ip(const char *ifname, const char *ip, const char *netmask)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return NN_ERRCODE_FAIL;
    }

    struct ifreq ifr;
    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    // Set IP address
    addr->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &addr->sin_addr) != 1)
    {
        close(sock);
        return NN_ERRCODE_FAIL;
    }

    if (ioctl(sock, SIOCSIFADDR, &ifr) < 0)
    {
        close(sock);
        return NN_ERRCODE_FAIL;
    }

    // Set netmask
    if (inet_pton(AF_INET, netmask, &addr->sin_addr) != 1)
    {
        close(sock);
        return NN_ERRCODE_FAIL;
    }

    if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0)
    {
        close(sock);
        return NN_ERRCODE_FAIL;
    }

    close(sock);
    return NN_ERRCODE_SUCCESS;
}

// Set interface state (up/down) - works for any type
int nn_if_set_state(const char *ifname, int up)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return NN_ERRCODE_FAIL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    // Get current flags
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0)
    {
        close(sock);
        return NN_ERRCODE_FAIL;
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
        return NN_ERRCODE_FAIL;
    }

    close(sock);
    return NN_ERRCODE_SUCCESS;
}

// Set MTU
int nn_if_set_mtu(const char *ifname, int mtu)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return NN_ERRCODE_FAIL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_mtu = mtu;

    if (ioctl(sock, SIOCSIFMTU, &ifr) < 0)
    {
        close(sock);
        return NN_ERRCODE_FAIL;
    }

    close(sock);
    return NN_ERRCODE_SUCCESS;
}
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

// Helper to add Netlink attributes
static void nn_if_add_attr(struct nlmsghdr *n, int maxlen, int type, const void *data, int alen)
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

// Create veth pair using Netlink
static int nn_if_create_veth_netlink(const char *ifname, const char *peer_name)
{
    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0)
    {
        perror("[if] socket(AF_NETLINK)");
        return NN_ERRCODE_FAIL;
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

    nn_if_add_attr(&req.n, sizeof(req), IFLA_IFNAME, ifname, strlen(ifname) + 1);

    // IFLA_LINKINFO
    struct rtattr *linkinfo = (struct rtattr *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
    nn_if_add_attr(&req.n, sizeof(req), IFLA_LINKINFO, NULL, 0);

    nn_if_add_attr(&req.n, sizeof(req), IFLA_INFO_KIND, "veth", 5);

    // IFLA_INFO_DATA
    struct rtattr *infodata = (struct rtattr *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
    nn_if_add_attr(&req.n, sizeof(req), IFLA_INFO_DATA, NULL, 0);

    // VETH_INFO_PEER
    struct rtattr *peerinfo = (struct rtattr *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
    nn_if_add_attr(&req.n, sizeof(req), 1, NULL, 0); // VETH_INFO_PEER is 1

    struct ifinfomsg peer_ifi;
    memset(&peer_ifi, 0, sizeof(peer_ifi));
    peer_ifi.ifi_family = AF_UNSPEC;

    // Append peer ifinfomsg to the VETH_INFO_PEER attribute
    int peer_ifi_len = sizeof(struct ifinfomsg);
    memcpy(RTA_DATA(peerinfo), &peer_ifi, peer_ifi_len);
    req.n.nlmsg_len += NLMSG_ALIGN(peer_ifi_len);
    peerinfo->rta_len += NLMSG_ALIGN(peer_ifi_len);

    // Add peer name as IFLA_IFNAME to the peer info
    nn_if_add_attr(&req.n, sizeof(req), IFLA_IFNAME, peer_name, strlen(peer_name) + 1);
    peerinfo->rta_len += RTA_ALIGN(RTA_LENGTH(strlen(peer_name) + 1));

    // Fix up lengths
    infodata->rta_len = (char *)(((char *)&req.n) + req.n.nlmsg_len) - (char *)infodata;
    linkinfo->rta_len = (char *)(((char *)&req.n) + req.n.nlmsg_len) - (char *)linkinfo;

    if (send(sock, &req, req.n.nlmsg_len, 0) < 0)
    {
        perror("[if] Netlink send");
        close(sock);
        return NN_ERRCODE_FAIL;
    }

    // Wait for ACK
    char ans[4096];
    int len = recv(sock, ans, sizeof(ans), 0);
    if (len < 0)
    {
        perror("[if] Netlink recv");
        close(sock);
        return NN_ERRCODE_FAIL;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)ans;
    if (nlh->nlmsg_type == NLMSG_ERROR)
    {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
        if (err->error < 0)
        {
            fprintf(stderr, "[if] Netlink error: %s\n", strerror(-err->error));
            close(sock);
            return NN_ERRCODE_FAIL;
        }
    }

    close(sock);
    return NN_ERRCODE_SUCCESS;
}

// Ensure interface exists (create if not)
int nn_if_ensure_exists(const char *ifname)
{
    if (nn_if_exists(ifname))
    {
        return NN_ERRCODE_SUCCESS;
    }

    printf("[if] Interface %s not found, attempting to create veth pair via Netlink...\n", ifname);

    char peer_name[IFNAMSIZ];
    snprintf(peer_name, sizeof(peer_name), "%s-peer", ifname);

    if (nn_if_create_veth_netlink(ifname, peer_name) != NN_ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[if] Error: Failed to create veth pair for %s (check CAP_NET_ADMIN)\n", ifname);
        return NN_ERRCODE_FAIL;
    }

    // Bring both up
    nn_if_set_state(ifname, 1);
    nn_if_set_state(peer_name, 1);

    printf("[if] Successfully created %s and its peer\n", ifname);
    return NN_ERRCODE_SUCCESS;
}
