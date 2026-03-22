/**
 * @file   if_map.c
 * @brief  接口映射实现（逻辑名→物理名，统一使用 GHashTable 管理所有接口类型）
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

/**
 * @brief 向 all_entries 哈希表插入一条接口条目（自动分配内存，接管所有权）
 */
static void if_map_insert(const char *logical, const char *physical, uint32_t auto_mapped)
{
    if (!g_interface_map.all_entries || !logical || !physical)
    {
        return;
    }

    /* 若已存在则更新物理名 */
    if_map_entry_t *existing = (if_map_entry_t *)g_hash_table_lookup(g_interface_map.all_entries, logical);
    if (existing)
    {
        if_map_copy_str(existing->physical_name, sizeof(existing->physical_name), physical);
        return;
    }

    if_map_entry_t *entry = (if_map_entry_t *)g_malloc0(sizeof(if_map_entry_t));
    if_map_copy_str(entry->logical_name, sizeof(entry->logical_name), logical);
    if_map_copy_str(entry->physical_name, sizeof(entry->physical_name), physical);
    entry->auto_mapped = auto_mapped;
    entry->ifindex = (uint32_t)if_nametoindex(physical);
    g_hash_table_insert(g_interface_map.all_entries, g_strdup(logical), entry);
}

/* 从配置文件加载 GE 口映射 */
static int load_config_file(const char *config_file)
{
    FILE *fp = fopen(config_file, "r");
    if (fp == NULL)
    {
        return ERRCODE_FAIL;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }

        char logical[LOGICAL_NAME_LEN];
        char physical[IFNAMSIZ];
        if (sscanf(line, " %31s = %15s", logical, physical) == 2)
        {
            if_map_insert(logical, physical, 0);
        }
    }

    fclose(fp);
    return ERRCODE_SUCCESS;
}

/* 初始化接口映射 */
int if_map_init(const char *config_file)
{
    if (config_file == NULL)
    {
        LOG_ERROR("No config file specified");
        return ERRCODE_FAIL;
    }

    /* 初始化统一哈希表（key 和 value 均由表负责释放） */
    if (!g_interface_map.all_entries)
    {
        g_interface_map.all_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    }

    /* 加载配置文件中的 GE 口映射 */
    int count_before = (int)g_hash_table_size(g_interface_map.all_entries);
    if (load_config_file(config_file) == ERRCODE_SUCCESS)
    {
        int loaded = (int)g_hash_table_size(g_interface_map.all_entries) - count_before;
        LOG_INFO("Loaded %d interface mapping(s) from %s", loaded, config_file);

        /* 确保物理接口存在 */
        GHashTableIter iter;
        gpointer key_ptr, val_ptr;
        g_hash_table_iter_init(&iter, g_interface_map.all_entries);
        while (g_hash_table_iter_next(&iter, &key_ptr, &val_ptr))
        {
            if_map_entry_t *e = (if_map_entry_t *)val_ptr;
            LOG_INFO("  %s -> %s", e->logical_name, e->physical_name);
            if_ensure_exists(e->physical_name);
            /* 刷新 ifindex（接口刚被创建，之前可能为 0） */
            e->ifindex = (uint32_t)if_nametoindex(e->physical_name);
        }
    }
    else
    {
        LOG_WARN("Failed to load config file %s", config_file);
    }

    /* null0 黑洞接口（固定单例，无 OS 接口） */
    if_map_insert("null0", "null0", 0);
    LOG_INFO("  null0 -> null0 (blackhole, virtual)");

    return ERRCODE_SUCCESS;
}

/* 按逻辑名查物理名 */
const char *if_map_get_physical(const char *logical_name)
{
    if (!logical_name || !g_interface_map.all_entries)
    {
        return logical_name;
    }
    if_map_entry_t *e = (if_map_entry_t *)g_hash_table_lookup(g_interface_map.all_entries, logical_name);
    return e ? e->physical_name : logical_name;
}

/* 按物理名查逻辑名（线性扫描，接口数量少，性能可接受） */
const char *if_map_get_logical(const char *physical_name)
{
    if (!physical_name || !g_interface_map.all_entries)
    {
        return physical_name;
    }
    GHashTableIter iter;
    gpointer key_ptr, val_ptr;
    g_hash_table_iter_init(&iter, g_interface_map.all_entries);
    while (g_hash_table_iter_next(&iter, &key_ptr, &val_ptr))
    {
        if_map_entry_t *e = (if_map_entry_t *)val_ptr;
        if (strcmp(e->physical_name, physical_name) == 0)
        {
            return e->logical_name;
        }
    }
    return physical_name;
}

/* 手动添加映射 */
int if_map_add(const char *logical_name, const char *physical_name)
{
    if (!logical_name || !physical_name || !g_interface_map.all_entries)
    {
        return ERRCODE_FAIL;
    }
    if_map_insert(logical_name, physical_name, 0);
    return ERRCODE_SUCCESS;
}

/* 保存映射到配置文件（仅保存非 loop/null0 的静态条目） */
int if_map_save(const char *config_file)
{
    FILE *fp = fopen(config_file, "w");
    if (fp == NULL)
    {
        return ERRCODE_FAIL;
    }

    fprintf(fp, "# NetNexus Interface Mapping Configuration\n");
    fprintf(fp, "# Format: logical_name = physical_name\n\n");

    if (g_interface_map.all_entries)
    {
        GHashTableIter iter;
        gpointer key_ptr, val_ptr;
        g_hash_table_iter_init(&iter, g_interface_map.all_entries);
        while (g_hash_table_iter_next(&iter, &key_ptr, &val_ptr))
        {
            if_map_entry_t *e = (if_map_entry_t *)val_ptr;
            /* 跳过 loop 接口和 null0（动态分配，不保存到静态配置） */
            if (strncmp(e->logical_name, "loop", 4) == 0 || strcmp(e->logical_name, "null0") == 0)
            {
                continue;
            }
            fprintf(fp, "%s = %s\n", e->logical_name, e->physical_name);
        }
    }

    fclose(fp);
    return ERRCODE_SUCCESS;
}
