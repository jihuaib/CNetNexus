#ifndef NN_INTERFACE_MAP_H
#define NN_INTERFACE_MAP_H

#include <net/if.h>
#include <stdint.h>

#define NN_MAX_INTERFACES 16
#define NN_LOGICAL_NAME_LEN 32

// Interface mapping entry
typedef struct
{
    char logical_name[NN_LOGICAL_NAME_LEN]; // e.g., "port0", "port1"
    char physical_name[IFNAMSIZ];           // e.g., "eth0", "veth0", "ens33"
} nn_interface_map_entry_t;

// Interface mapping table
typedef struct
{
    nn_interface_map_entry_t entries[NN_MAX_INTERFACES];
    int count;
} nn_interface_map_t;

// Global interface map
extern nn_interface_map_t g_interface_map;

// Initialize interface mapping (auto-detect or load from config)
int nn_interface_map_init(const char *config_file);

// Get physical interface name from logical name
const char *nn_interface_map_get_physical(const char *logical_name);

// Get logical interface name from physical name
const char *nn_interface_map_get_logical(const char *physical_name);

// Add manual mapping
int nn_interface_map_add(const char *logical_name, const char *physical_name);

// List all mappings
void nn_interface_map_list(int client_fd);

// Save mappings to config file
int nn_interface_map_save(const char *config_file);

#endif // NN_INTERFACE_MAP_H
