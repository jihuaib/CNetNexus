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

#include "net_addr.h"

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

// Get detailed interface information
int if_get_info(const char *ifname, if_info_t *info);

// Set interface state (up/down)
int if_set_state(const char *ifname, int up);

// Set MTU
int if_set_mtu(const char *ifname, int mtu);

// Check if interface exists
int if_exists(const char *ifname);

// Ensure interface exists (create if not)
int if_ensure_exists(const char *ifname);

// 创建 dummy 接口（用于 loop 接口模拟）
int if_create_dummy(const char *ifname);

// 删除接口（通过 Netlink RTM_DELLINK）
int if_delete_interface(const char *ifname);

// 在网卡上配置/撤销 IP 前缀（RTM_NEWADDR/RTM_DELADDR）
int if_addr_add_prefix(const char *ifname, const net_prefix_t *prefix);
int if_addr_del_prefix(const char *ifname, const net_prefix_t *prefix);

// 通过 Netlink 下发 Linux 黑洞路由（RTN_BLACKHOLE）
int if_blackhole_route_add(sa_family_t family, const void *prefix_bin, uint8_t prefix_len);

// 撤销 Linux 黑洞路由
int if_blackhole_route_del(sa_family_t family, const void *prefix_bin, uint8_t prefix_len);

#endif // IF_H
