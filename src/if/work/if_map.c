/**
 * @file   if_map.c
 * @brief  接口映射实现（逻辑名→物理名，统一使用 GTree 有序管理所有接口类型）
 * @author jhb
 * @date   2026/01/22
 */
#include "if_map.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "errcode.h"
#include "if_netlink.h"
#include "if_worker.h"
#include "log.h"

/* 使用 g_if_work_local->interface_map，不再维护独立全局变量 */
#define g_interface_map (g_if_work_local->interface_map)
#define IF_MAP_IFINDEX_RETRY_COUNT 20u
#define IF_MAP_IFINDEX_RETRY_USEC 50000u

/* 接口名类型优先级：null0(0) → loop(1) → GE(2) → 其他(3) */
static int if_name_type_order(const char *name)
{
    if (strncmp(name, "null", 4) == 0)
    {
        return 0;
    }
    if (strncmp(name, "loop", 4) == 0)
    {
        return 1;
    }
    if (strncmp(name, "GE-", 3) == 0)
    {
        return 2;
    }
    return 3;
}

/* 从接口名末尾提取数字 */
static int if_name_extract_number(const char *name)
{
    const char *p = name + strlen(name);
    while (p > name && g_ascii_isdigit(*(p - 1)))
    {
        p--;
    }
    return (*p != '\0') ? atoi(p) : 0;
}

/**
 * @brief GTree 键比较函数
 *
 * 排序规则：null0 → loopN（按编号升序）→ GE-N（按编号升序）→ 其他（字典序）
 */
static gint if_map_key_compare(gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    const char *na = (const char *)a;
    const char *nb = (const char *)b;

    int order_a = if_name_type_order(na);
    int order_b = if_name_type_order(nb);
    if (order_a != order_b)
    {
        return order_a - order_b;
    }

    int num_a = if_name_extract_number(na);
    int num_b = if_name_extract_number(nb);
    if (num_a != num_b)
    {
        return num_a - num_b;
    }

    return strcmp(na, nb);
}

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

gboolean if_map_is_virtual_entry(const char *logical_name)
{
    return (logical_name && strcmp(logical_name, "null0") == 0) ? TRUE : FALSE;
}

static uint32_t if_map_wait_ifindex(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return 0u;
    }

    for (uint32_t i = 0u; i < IF_MAP_IFINDEX_RETRY_COUNT; ++i)
    {
        uint32_t ifindex = (uint32_t)if_nametoindex(ifname);
        if (ifindex != 0u)
        {
            return ifindex;
        }
        (void)usleep(IF_MAP_IFINDEX_RETRY_USEC);
    }
    return 0u;
}

/**
 * @brief 向 all_entries 有序树插入一条接口条目（自动分配内存，接管所有权）
 */
static int if_map_insert(const char *logical, const char *physical, uint32_t auto_mapped)
{
    if (!g_interface_map.all_entries || !logical || !physical)
    {
        return ERRCODE_FAIL;
    }

    uint32_t ifindex = 0u;
    if (!if_map_is_virtual_entry(logical))
    {
        (void)if_ensure_exists(physical);
        ifindex = if_map_wait_ifindex(physical);
        if (ifindex == 0u)
        {
            LOG_ERROR("IF: interface %s(%s) ifindex invalid(0), skip mapping", logical, physical);
            return ERRCODE_FAIL;
        }
    }

    /* 若已存在则更新物理名 */
    if_map_entry_t *existing = (if_map_entry_t *)g_tree_lookup(g_interface_map.all_entries, logical);
    if (existing)
    {
        if_map_copy_str(existing->physical_name, sizeof(existing->physical_name), physical);
        existing->ifindex = ifindex;
        return ERRCODE_SUCCESS;
    }

    if_map_entry_t *entry = (if_map_entry_t *)g_malloc0(sizeof(if_map_entry_t));
    if_map_copy_str(entry->logical_name, sizeof(entry->logical_name), logical);
    if_map_copy_str(entry->physical_name, sizeof(entry->physical_name), physical);
    entry->auto_mapped = auto_mapped;
    entry->ifindex = ifindex;

    if (if_map_is_virtual_entry(logical))
    {
        /* null0 固定虚拟黑洞接口：默认管理/链路都视为 UP */
        entry->shutdown = 0;
        entry->link_up = 1;
    }
    else
    {
        /* 默认物理接口管理状态为 DOWN */
        entry->shutdown = 1;
        entry->link_up = -1;
        (void)if_set_state(physical, 0);
    }

    g_tree_insert(g_interface_map.all_entries, g_strdup(logical), entry);
    return ERRCODE_SUCCESS;
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
            if (if_map_insert(logical, physical, 0) != ERRCODE_SUCCESS)
            {
                LOG_WARN("IF: skip invalid mapping %s=%s from %s", logical, physical, config_file);
            }
        }
    }

    fclose(fp);
    return ERRCODE_SUCCESS;
}

/* GTree 遍历回调：确保物理接口存在并刷新 ifindex */
static gboolean if_map_init_ensure_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    (void)user_data;
    if_map_entry_t *e = (if_map_entry_t *)value;
    LOG_INFO("  %s -> %s", e->logical_name, e->physical_name);
    if (!if_map_is_virtual_entry(e->logical_name))
    {
        (void)if_ensure_exists(e->physical_name);
        uint32_t ifindex = if_map_wait_ifindex(e->physical_name);
        if (ifindex != 0u)
        {
            e->ifindex = ifindex;
        }
        else
        {
            LOG_ERROR("IF: interface %s(%s) ifindex invalid(0) after ensure", e->logical_name, e->physical_name);
        }
    }
    return FALSE;
}

/* 初始化接口映射 */
int if_map_init(const char *config_file)
{
    if (config_file == NULL)
    {
        LOG_ERROR("No config file specified");
        return ERRCODE_FAIL;
    }

    /* 初始化有序树（key 和 value 均由树负责释放） */
    if (!g_interface_map.all_entries)
    {
        g_interface_map.all_entries = g_tree_new_full(if_map_key_compare, NULL, g_free, g_free);
    }

    /* 加载配置文件中的 GE 口映射 */
    int count_before = (int)g_tree_nnodes(g_interface_map.all_entries);
    if (load_config_file(config_file) == ERRCODE_SUCCESS)
    {
        int loaded = (int)g_tree_nnodes(g_interface_map.all_entries) - count_before;
        LOG_INFO("Loaded %d interface mapping(s) from %s", loaded, config_file);

        /* 确保物理接口存在 */
        g_tree_foreach(g_interface_map.all_entries, if_map_init_ensure_cb, NULL);
    }
    else
    {
        LOG_WARN("Failed to load config file %s", config_file);
    }

    /* null0 黑洞接口（固定单例，无 OS 接口） */
    (void)if_map_insert("null0", "null0", 0);
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
    if_map_entry_t *e = (if_map_entry_t *)g_tree_lookup(g_interface_map.all_entries, logical_name);
    return e ? e->physical_name : logical_name;
}

/* 按物理名查逻辑名的遍历上下文 */
typedef struct
{
    const char *physical_name;
    const char *result;
} if_map_find_by_physical_t;

/* GTree 遍历回调：按物理名查找 */
static gboolean if_map_find_physical_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    if_map_entry_t *e = (if_map_entry_t *)value;
    if_map_find_by_physical_t *ctx = (if_map_find_by_physical_t *)user_data;
    if (strcmp(e->physical_name, ctx->physical_name) == 0)
    {
        ctx->result = e->logical_name;
        return TRUE; /* 找到，停止遍历 */
    }
    return FALSE;
}

/* 按物理名查逻辑名（线性扫描，接口数量少，性能可接受） */
const char *if_map_get_logical(const char *physical_name)
{
    if (!physical_name || !g_interface_map.all_entries)
    {
        return physical_name;
    }
    if_map_find_by_physical_t ctx = {.physical_name = physical_name, .result = NULL};
    g_tree_foreach(g_interface_map.all_entries, if_map_find_physical_cb, &ctx);
    return ctx.result ? ctx.result : physical_name;
}

/* GTree 遍历回调：保存静态映射（跳过 loop/null0 和自动映射条目） */
static gboolean if_map_save_foreach_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    if_map_entry_t *e = (if_map_entry_t *)value;
    FILE *fp = (FILE *)user_data;
    if (!e || !fp)
    {
        return FALSE;
    }

    if (strncmp(e->logical_name, "loop", 4) == 0 || strcmp(e->logical_name, "null0") == 0 || e->auto_mapped)
    {
        return FALSE;
    }

    fprintf(fp, "%s = %s\n", e->logical_name, e->physical_name);
    return FALSE;
}

/* 手动添加映射 */
int if_map_add(const char *logical_name, const char *physical_name)
{
    if (!logical_name || !physical_name || !g_interface_map.all_entries)
    {
        return ERRCODE_FAIL;
    }
    return if_map_insert(logical_name, physical_name, 0);
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
        g_tree_foreach(g_interface_map.all_entries, if_map_save_foreach_cb, fp);
    }

    fclose(fp);
    return ERRCODE_SUCCESS;
}
