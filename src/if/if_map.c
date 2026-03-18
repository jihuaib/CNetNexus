/**
 * @file   if_map.c
 * @brief  接口映射实现，逻辑接口名到物理接口名的映射
 * @author jhb
 * @date   2026/01/22
 */
#include "if_map.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errcode.h"
#include "if.h"
#include "if_main.h"
#include "log.h"

/* 使用 g_if_local->interface_map，不再维护独立全局变量 */
#define g_interface_map (g_if_local->interface_map)

static void if_map_copy_str(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
    {
        return;
    }
    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    size_t len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// Load mappings from config file
static int load_config_file(const char *config_file)
{
    FILE *fp = fopen(config_file, "r");
    if (fp == NULL)
    {
        return ERRCODE_FAIL;
    }

    char line[256];
    g_interface_map.count = 0;

    while (fgets(line, sizeof(line), fp) && g_interface_map.count < MAX_INTERFACES)
    {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }

        // Parse: logical_name = physical_name
        char logical[LOGICAL_NAME_LEN];
        char physical[IFNAMSIZ];

        if (sscanf(line, " %31s = %15s", logical, physical) == 2)
        {
            // Add mapping
            if_map_copy_str(g_interface_map.entries[g_interface_map.count].logical_name,
                            sizeof(g_interface_map.entries[g_interface_map.count].logical_name), logical);
            if_map_copy_str(g_interface_map.entries[g_interface_map.count].physical_name,
                            sizeof(g_interface_map.entries[g_interface_map.count].physical_name), physical);
            g_interface_map.entries[g_interface_map.count].auto_mapped = 0;
            g_interface_map.count++;
        }
    }

    fclose(fp);
    return ERRCODE_SUCCESS;
}

// Initialize interface mapping
int if_map_init(const char *config_file)
{
    if (config_file == NULL)
    {
        LOG_ERROR("No config file specified");
        return ERRCODE_FAIL;
    }

    // Load from config file
    if (load_config_file(config_file) == ERRCODE_SUCCESS)
    {
        LOG_INFO("Loaded %d interface mapping(s) from %s", g_interface_map.count, config_file);

        // Print mappings and ensure interfaces exist
        for (int i = 0; i < g_interface_map.count; i++)
        {
            LOG_INFO("  %s -> %s", g_interface_map.entries[i].logical_name, g_interface_map.entries[i].physical_name);

            // Ensure the physical interface exists (create if virtual)
            if_ensure_exists(g_interface_map.entries[i].physical_name);
        }

        return ERRCODE_SUCCESS;
    }

    LOG_WARN("Failed to load config file %s", config_file);
    return ERRCODE_FAIL;
}

// Get physical interface name from logical name
const char *if_map_get_physical(const char *logical_name)
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
const char *if_map_get_logical(const char *physical_name)
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
int if_map_add(const char *logical_name, const char *physical_name)
{
    if (g_interface_map.count >= MAX_INTERFACES)
    {
        return ERRCODE_FAIL;
    }

    // Check if logical name already exists
    for (int i = 0; i < g_interface_map.count; i++)
    {
        if (strcmp(g_interface_map.entries[i].logical_name, logical_name) == 0)
        {
            // Update existing mapping
            if_map_copy_str(g_interface_map.entries[i].physical_name, sizeof(g_interface_map.entries[i].physical_name),
                            physical_name);
            return ERRCODE_SUCCESS;
        }
    }

    // Add new mapping
    if_map_copy_str(g_interface_map.entries[g_interface_map.count].logical_name,
                    sizeof(g_interface_map.entries[g_interface_map.count].logical_name), logical_name);
    if_map_copy_str(g_interface_map.entries[g_interface_map.count].physical_name,
                    sizeof(g_interface_map.entries[g_interface_map.count].physical_name), physical_name);
    g_interface_map.count++;

    return ERRCODE_SUCCESS;
}

// Save mappings to config file
int if_map_save(const char *config_file)
{
    FILE *fp = fopen(config_file, "w");
    if (fp == NULL)
    {
        return ERRCODE_FAIL;
    }

    fprintf(fp, "# NetNexus Interface Mapping Configuration\n");
    fprintf(fp, "# Format: logical_name = physical_name\n");
    fprintf(fp, "# Use 'auto' for automatic detection\n\n");

    for (int i = 0; i < g_interface_map.count; i++)
    {
        fprintf(fp, "%s = %s\n", g_interface_map.entries[i].logical_name, g_interface_map.entries[i].physical_name);
    }

    fclose(fp);
    return ERRCODE_SUCCESS;
}
