#ifndef NN_IF_H
#define NN_IF_H

#include <arpa/inet.h>
#include <net/if.h>
#include <stdint.h>

// Interface types
typedef enum
{
    NN_IF_TYPE_UNKNOWN = 0,
    NN_IF_TYPE_ETHERNET, // Physical Ethernet (eth0, ens33, etc.)
    NN_IF_TYPE_VETH,     // Virtual Ethernet pair (veth0, etc.)
    NN_IF_TYPE_LOOPBACK, // Loopback (lo)
    NN_IF_TYPE_BRIDGE,   // Bridge (br0, docker0, etc.)
    NN_IF_TYPE_TUN,      // TUN/TAP
    NN_IF_TYPE_VLAN,     // VLAN
} nn_if_type_t;

// Interface state
typedef enum
{
    NN_IF_STATE_DOWN = 0,
    NN_IF_STATE_UP,
    NN_IF_STATE_UNKNOWN,
} nn_if_state_t;

// Interface information structure
typedef struct
{
    char name[IFNAMSIZ];              // Interface name (e.g., "eth0", "veth0")
    nn_if_type_t type;                // Interface type
    nn_if_state_t state;              // Up/Down state
    uint32_t flags;                   // Interface flags (IFF_*)
    char ip_address[INET_ADDRSTRLEN]; // IPv4 address
    char netmask[INET_ADDRSTRLEN];    // Netmask
    uint8_t mac[6];                   // MAC address
    int mtu;                          // MTU
    uint64_t rx_bytes;                // RX statistics
    uint64_t tx_bytes;                // TX statistics
} nn_if_info_t;

// Interface abstraction layer
// These functions work with any interface type (eth, veth, etc.)

// Detect interface type
nn_if_type_t nn_if_detect_type(const char *ifname);

// Get interface type as string
const char *nn_if_type_to_string(nn_if_type_t type);

// List all available interfaces
int nn_if_list(nn_if_info_t **interfaces, int *count);

// Get detailed interface information
int nn_if_get_info(const char *ifname, nn_if_info_t *info);

// Set IP address (works for any interface type)
int nn_if_set_ip(const char *ifname, const char *ip, const char *netmask);

// Set interface state (up/down)
int nn_if_set_state(const char *ifname, int up);

// Set MTU
int nn_if_set_mtu(const char *ifname, int mtu);

// Check if interface exists
int nn_if_exists(const char *ifname);

// Ensure interface exists (create if not)
int nn_if_ensure_exists(const char *ifname);

// Global interface context
extern char g_current_interface[IFNAMSIZ];

#endif // NN_IF_H
