/**
 * @file   if.h
 * @brief  接口配置模块头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef IF_H
#define IF_H

#include <arpa/inet.h>
#include <net/if.h>
#include <stdint.h>

// Interface types
typedef enum
{
    IF_TYPE_UNKNOWN = 0,
    IF_TYPE_ETHERNET, // Physical Ethernet (eth0, ens33, etc.)
    IF_TYPE_VETH,     // Virtual Ethernet pair (veth0, etc.)
    IF_TYPE_LOOPBACK, // Loopback (lo)
    IF_TYPE_BRIDGE,   // Bridge (br0, docker0, etc.)
    IF_TYPE_TUN,      // TUN/TAP
    IF_TYPE_VLAN,     // VLAN
} if_type_t;

// Interface state
typedef enum
{
    IF_STATE_DOWN = 0,
    IF_STATE_UP,
    IF_STATE_UNKNOWN,
} if_state_t;

// Interface information structure
typedef struct
{
    char name[IFNAMSIZ];              // Interface name (e.g., "eth0", "veth0")
    if_type_t type;                   // Interface type
    if_state_t state;                 // Up/Down state
    uint32_t flags;                   // Interface flags (IFF_*)
    char ip_address[INET_ADDRSTRLEN]; // IPv4 address
    char netmask[INET_ADDRSTRLEN];    // Netmask
    uint8_t mac[6];                   // MAC address
    int mtu;                          // MTU
    uint64_t rx_bytes;                // RX statistics
    uint64_t tx_bytes;                // TX statistics
} if_info_t;

// Interface abstraction layer
// These functions work with any interface type (eth, veth, etc.)

// Detect interface type
if_type_t if_detect_type(const char *ifname);

// Get interface type as string
const char *if_type_to_string(if_type_t type);

// List all available interfaces
int if_list(if_info_t **interfaces, int *count);

// Get detailed interface information
int if_get_info(const char *ifname, if_info_t *info);

// Set IP address (works for any interface type)
int if_set_ip(const char *ifname, const char *ip, const char *netmask);

// Set interface state (up/down)
int if_set_state(const char *ifname, int up);

// Set MTU
int if_set_mtu(const char *ifname, int mtu);

// Check if interface exists
int if_exists(const char *ifname);

// Ensure interface exists (create if not)
int if_ensure_exists(const char *ifname);

// Global interface context
extern char g_current_interface[IFNAMSIZ];

#endif // IF_H
