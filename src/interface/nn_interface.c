// Interface configuration module for NetNexus
// Provides abstraction layer for different interface types

#include "nn_interface.h"

#include <arpa/inet.h>
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
#include "nn_interface_map.h"

// Current interface context (for interface mode)
static char g_current_interface[IFNAMSIZ] = {0};

// Detect interface type by reading /sys/class/net/<ifname>/type
nn_interface_type_t nn_interface_detect_type(const char *ifname)
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
const char *nn_interface_type_to_string(nn_interface_type_t type)
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
int nn_interface_list(nn_interface_info_t **interfaces, int *count)
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

    *interfaces = calloc(n, sizeof(nn_interface_info_t));
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
            nn_interface_get_info(ifa->ifa_name, &(*interfaces)[idx]);
            idx++;
        }
    }

    *count = n;
    freeifaddrs(ifaddr);
    return NN_ERRCODE_SUCCESS;
}

// Get detailed interface information
int nn_interface_get_info(const char *ifname, nn_interface_info_t *info)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return NN_ERRCODE_FAIL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    memset(info, 0, sizeof(nn_interface_info_t));
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
    info->type = nn_interface_detect_type(ifname);

    // TODO: Get RX/TX statistics from /sys/class/net/<ifname>/statistics/

    return NN_ERRCODE_SUCCESS;
}

// Check if interface exists
int nn_interface_exists(const char *ifname)
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
int nn_interface_set_ip(const char *ifname, const char *ip, const char *netmask)
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
int nn_interface_set_state(const char *ifname, int up)
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
int nn_interface_set_mtu(const char *ifname, int mtu)
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

// ============================================================================
// CLI Command Handlers
// ============================================================================

// Command: interface <name>
int nn_cmd_interface(int client_fd, int argc, char **argv)
{
    if (argc < 2)
    {
        dprintf(client_fd, "Error: Interface name required\r\n");
        return NN_ERRCODE_FAIL;
    }

    const char *logical_name = argv[1];

    // Resolve logical name to physical name
    const char *physical_name = nn_interface_map_get_physical(logical_name);

    // Check if interface exists
    if (!nn_interface_exists(physical_name))
    {
        dprintf(client_fd, "Error: Interface %s (mapped to %s) does not exist\r\n", logical_name, physical_name);
        return NN_ERRCODE_FAIL;
    }

    // Get interface type
    nn_interface_type_t type = nn_interface_detect_type(physical_name);
    const char *type_str = nn_interface_type_to_string(type);

    // Enter interface configuration mode
    strncpy(g_current_interface, physical_name, IFNAMSIZ - 1);

    if (strcmp(logical_name, physical_name) == 0)
    {
        // Direct physical name used
        dprintf(client_fd, "Entering interface configuration mode for %s (%s)\r\n", physical_name, type_str);
    }
    else
    {
        // Logical name mapped
        dprintf(client_fd, "Entering interface configuration mode for %s -> %s (%s)\r\n", logical_name, physical_name,
                type_str);
    }

    return NN_ERRCODE_SUCCESS;
}

// Command: ip address <ip> <netmask>
int nn_cmd_ip_address(int client_fd, int argc, char **argv)
{
    if (argc < 4)
    {
        dprintf(client_fd, "Error: Usage: ip address <ip-address> <netmask>\r\n");
        return NN_ERRCODE_FAIL;
    }

    if (g_current_interface[0] == '\0')
    {
        dprintf(client_fd, "Error: Not in interface configuration mode\r\n");
        return NN_ERRCODE_FAIL;
    }

    const char *ip = argv[2];
    const char *netmask = argv[3];

    if (nn_interface_set_ip(g_current_interface, ip, netmask) == NN_ERRCODE_SUCCESS)
    {
        dprintf(client_fd, "IP address %s %s configured on %s\r\n", ip, netmask, g_current_interface);
        return NN_ERRCODE_SUCCESS;
    }

    dprintf(client_fd, "Error: Failed to configure IP address\r\n");
    return NN_ERRCODE_FAIL;
}

// Command: shutdown
int nn_cmd_shutdown(int client_fd, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (g_current_interface[0] == '\0')
    {
        dprintf(client_fd, "Error: Not in interface configuration mode\r\n");
        return NN_ERRCODE_FAIL;
    }

    if (nn_interface_set_state(g_current_interface, 0) == NN_ERRCODE_SUCCESS)
    {
        dprintf(client_fd, "Interface %s shutdown\r\n", g_current_interface);
        return NN_ERRCODE_SUCCESS;
    }

    dprintf(client_fd, "Error: Failed to shutdown interface\r\n");
    return NN_ERRCODE_FAIL;
}

// Command: no shutdown
int nn_cmd_no_shutdown(int client_fd, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (g_current_interface[0] == '\0')
    {
        dprintf(client_fd, "Error: Not in interface configuration mode\r\n");
        return NN_ERRCODE_FAIL;
    }

    if (nn_interface_set_state(g_current_interface, 1) == NN_ERRCODE_SUCCESS)
    {
        dprintf(client_fd, "Interface %s enabled\r\n", g_current_interface);
        return NN_ERRCODE_SUCCESS;
    }

    dprintf(client_fd, "Error: Failed to enable interface\r\n");
    return NN_ERRCODE_FAIL;
}

// Command: show interface [name]
int nn_cmd_show_interface(int client_fd, int argc, char **argv)
{
    nn_interface_info_t *interfaces = NULL;
    int count = 0;

    if (argc >= 3)
    {
        // Show specific interface
        const char *ifname = argv[2];
        nn_interface_info_t info;

        if (nn_interface_get_info(ifname, &info) != NN_ERRCODE_SUCCESS)
        {
            dprintf(client_fd, "Error: Interface %s not found\r\n", ifname);
            return NN_ERRCODE_FAIL;
        }

        dprintf(client_fd, "Interface %s:\r\n", info.name);
        dprintf(client_fd, "  Type: %s\r\n", nn_interface_type_to_string(info.type));
        dprintf(client_fd, "  State: %s\r\n", info.state == NN_IF_STATE_UP ? "UP" : "DOWN");
        dprintf(client_fd, "  IP: %s\r\n", info.ip_address[0] ? info.ip_address : "not configured");
        dprintf(client_fd, "  Netmask: %s\r\n", info.netmask[0] ? info.netmask : "not configured");
        dprintf(client_fd, "  MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n", info.mac[0], info.mac[1], info.mac[2],
                info.mac[3], info.mac[4], info.mac[5]);
        dprintf(client_fd, "  MTU: %d\r\n", info.mtu);
    }
    else
    {
        // Show all interfaces
        if (nn_interface_list(&interfaces, &count) != NN_ERRCODE_SUCCESS)
        {
            dprintf(client_fd, "Error: Failed to list interfaces\r\n");
            return NN_ERRCODE_FAIL;
        }

        dprintf(client_fd, "Interface Status:\r\n");
        dprintf(client_fd, "%-10s %-15s %-10s %-15s\r\n", "Name", "Type", "State", "IP Address");
        dprintf(client_fd, "%-10s %-15s %-10s %-15s\r\n", "----", "----", "-----", "----------");

        for (int i = 0; i < count; i++)
        {
            dprintf(client_fd, "%-10s %-15s %-10s %-15s\r\n", interfaces[i].name,
                    nn_interface_type_to_string(interfaces[i].type),
                    interfaces[i].state == NN_IF_STATE_UP ? "UP" : "DOWN",
                    interfaces[i].ip_address[0] ? interfaces[i].ip_address : "-");
        }

        free(interfaces);
    }

    return NN_ERRCODE_SUCCESS;
}

// Command: show ip interface [name]
int nn_cmd_show_ip_interface(int client_fd, int argc, char **argv)
{
    // Similar to show interface but IP-focused
    return nn_cmd_show_interface(client_fd, argc, argv);
}
