/**
 * @file   nn_if_map.c
 * @brief  接口映射实现，逻辑接口名到物理接口名的映射
 * @author jhb
 * @date   2026/01/22
 */
#include "nn_if_map.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_errcode.h"
#include "nn_if.h"

// Global interface mapping table
nn_if_map_t g_interface_map = {0};

// Load mappings from config file
static int load_config_file(const char *config_file)
{
    FILE *fp = fopen(config_file, "r");
    if (fp == NULL)
    {
        return NN_ERRCODE_FAIL;
    }

    char line[256];
    g_interface_map.count = 0;

    while (fgets(line, sizeof(line), fp) && g_interface_map.count < NN_MAX_INTERFACES)
    {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }

        // Parse: logical_name = physical_name
        char logical[NN_LOGICAL_NAME_LEN];
        char physical[IFNAMSIZ];

        if (sscanf(line, " %31s = %15s", logical, physical) == 2)
        {
            // Add mapping
            strncpy(g_interface_map.entries[g_interface_map.count].logical_name, logical, NN_LOGICAL_NAME_LEN - 1);
            strncpy(g_interface_map.entries[g_interface_map.count].physical_name, physical, IFNAMSIZ - 1);
            g_interface_map.entries[g_interface_map.count].auto_mapped = 0;
            g_interface_map.count++;
        }
    }

    fclose(fp);
    return NN_ERRCODE_SUCCESS;
}

// Initialize interface mapping
int nn_if_map_init(const char *config_file)
{
    if (config_file == NULL)
    {
        printf("[interface] Error: No config file specified\n");
        return NN_ERRCODE_FAIL;
    }

    // Load from config file
    if (load_config_file(config_file) == NN_ERRCODE_SUCCESS)
    {
        printf("[interface] Loaded %d interface mapping(s) from %s\n", g_interface_map.count, config_file);

        // Print mappings and ensure interfaces exist
        for (int i = 0; i < g_interface_map.count; i++)
        {
            printf("[interface]   %s -> %s\n", g_interface_map.entries[i].logical_name,
                   g_interface_map.entries[i].physical_name);

            // Ensure the physical interface exists (create if virtual)
            nn_if_ensure_exists(g_interface_map.entries[i].physical_name);
        }

        return NN_ERRCODE_SUCCESS;
    }

    printf("[interface] Warning: Failed to load config file %s\n", config_file);
    return NN_ERRCODE_FAIL;
}

// Get physical interface name from logical name
const char *nn_if_map_get_physical(const char *logical_name)
{
    for (int i = 0; i < g_interface_map.count; i++)
    {
        if (strcmp(g_interface_map.entries[i].logical_name, logical_name) == 0)
        {
            return g_interface_map.entries[i].physical_name;
        }
    }

    // If not found, assume it's already a physical name
    return logical_name;
}

// Get logical interface name from physical name
const char *nn_if_map_get_logical(const char *physical_name)
{
    for (int i = 0; i < g_interface_map.count; i++)
    {
        if (strcmp(g_interface_map.entries[i].physical_name, physical_name) == 0)
        {
            return g_interface_map.entries[i].logical_name;
        }
    }

    return physical_name;
}

// Add manual mapping
int nn_if_map_add(const char *logical_name, const char *physical_name)
{
    if (g_interface_map.count >= NN_MAX_INTERFACES)
    {
        return NN_ERRCODE_FAIL;
    }

    // Check if logical name already exists
    for (int i = 0; i < g_interface_map.count; i++)
    {
        if (strcmp(g_interface_map.entries[i].logical_name, logical_name) == 0)
        {
            // Update existing mapping
            strncpy(g_interface_map.entries[i].physical_name, physical_name, IFNAMSIZ - 1);
            return NN_ERRCODE_SUCCESS;
        }
    }

    // Add new mapping
    strncpy(g_interface_map.entries[g_interface_map.count].logical_name, logical_name, NN_LOGICAL_NAME_LEN - 1);
    strncpy(g_interface_map.entries[g_interface_map.count].physical_name, physical_name, IFNAMSIZ - 1);
    g_interface_map.count++;

    return NN_ERRCODE_SUCCESS;
}

// Save mappings to config file
int nn_if_map_save(const char *config_file)
{
    FILE *fp = fopen(config_file, "w");
    if (fp == NULL)
    {
        return NN_ERRCODE_FAIL;
    }

    fprintf(fp, "# NetNexus Interface Mapping Configuration\n");
    fprintf(fp, "# Format: logical_name = physical_name\n");
    fprintf(fp, "# Use 'auto' for automatic detection\n\n");

    for (int i = 0; i < g_interface_map.count; i++)
    {
        fprintf(fp, "%s = %s\n", g_interface_map.entries[i].logical_name, g_interface_map.entries[i].physical_name);
    }

    fclose(fp);
    return NN_ERRCODE_SUCCESS;
}
