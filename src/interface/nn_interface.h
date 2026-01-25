#ifndef NN_INTERFACE_H
#define NN_INTERFACE_H

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
} nn_interface_type_t;

// Interface state
typedef enum
{
    NN_IF_STATE_DOWN = 0,
    NN_IF_STATE_UP,
    NN_IF_STATE_UNKNOWN,
} nn_interface_state_t;

// Interface information structure
typedef struct
{
    char name[IFNAMSIZ];              // Interface name (e.g., "eth0", "veth0")
    nn_interface_type_t type;         // Interface type
    nn_interface_state_t state;       // Up/Down state
    uint32_t flags;                   // Interface flags (IFF_*)
    char ip_address[INET_ADDRSTRLEN]; // IPv4 address
    char netmask[INET_ADDRSTRLEN];    // Netmask
    uint8_t mac[6];                   // MAC address
    int mtu;                          // MTU
    uint64_t rx_bytes;                // RX statistics
    uint64_t tx_bytes;                // TX statistics
} nn_interface_info_t;

// Interface abstraction layer
// These functions work with any interface type (eth, veth, etc.)

// Detect interface type
nn_interface_type_t nn_interface_detect_type(const char *ifname);

// Get interface type as string
const char *nn_interface_type_to_string(nn_interface_type_t type);

// List all available interfaces
int nn_interface_list(nn_interface_info_t **interfaces, int *count);

// Get detailed interface information
int nn_interface_get_info(const char *ifname, nn_interface_info_t *info);

// Set IP address (works for any interface type)
int nn_interface_set_ip(const char *ifname, const char *ip, const char *netmask);

// Set interface state (up/down)
int nn_interface_set_state(const char *ifname, int up);

// Set MTU
int nn_interface_set_mtu(const char *ifname, int mtu);

// Check if interface exists
int nn_interface_exists(const char *ifname);

// CLI command handlers
int nn_cmd_interface(int client_fd, int argc, char **argv);
int nn_cmd_ip_address(int client_fd, int argc, char **argv);
int nn_cmd_shutdown(int client_fd, int argc, char **argv);
int nn_cmd_no_shutdown(int client_fd, int argc, char **argv);
int nn_cmd_show_interface(int client_fd, int argc, char **argv);
int nn_cmd_show_ip_interface(int client_fd, int argc, char **argv);

#endif // NN_INTERFACE_H
