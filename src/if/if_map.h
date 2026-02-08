/**
 * @file   if_map.h
 * @brief  接口映射头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef IF_MAP_H
#define IF_MAP_H

#include <net/if.h>
#include <stdint.h>

#define MAX_INTERFACES 16
#define LOGICAL_NAME_LEN 32

// Interface mapping entry
typedef struct
{
    char logical_name[LOGICAL_NAME_LEN]; // e.g., "port0", "port1"
    char physical_name[IFNAMSIZ];           // e.g., "eth0", "veth0", "ens33"
    uint32_t auto_mapped;
} if_map_entry_t;

// Interface mapping table
typedef struct
{
    if_map_entry_t entries[MAX_INTERFACES];
    int count;
} if_map_t;

// Global interface map
extern if_map_t g_interface_map;

// Initialize interface mapping (auto-detect or load from config)
int if_map_init(const char *config_file);

// Get physical interface name from logical name
const char *if_map_get_physical(const char *logical_name);

// Get logical interface name from physical name
const char *if_map_get_logical(const char *physical_name);

// Add manual mapping
int if_map_add(const char *logical_name, const char *physical_name);

// List all mappings
void if_map_list(int client_fd);

// Save mappings to config file
int if_map_save(const char *config_file);

#endif // IF_MAP_H
