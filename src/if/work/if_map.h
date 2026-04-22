/**
 * @file   if_map.h
 * @brief  接口映射头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef IF_MAP_H
#define IF_MAP_H

#include <glib.h>
#include <net/if.h>
#include <stdint.h>

#include "net_addr.h"

#define LOGICAL_NAME_LEN 32

/** 接口映射条目（同时作为接口内存态配置） */
typedef struct
{
    char logical_name[LOGICAL_NAME_LEN]; /**< 逻辑名，如 "GE-1" */
    char physical_name[IFNAMSIZ];        /**< 物理名，如 "eth0" */
    uint32_t ifindex;                    /**< Linux 接口索引（仅虚拟接口如 null0 允许为 0） */
    uint32_t auto_mapped;                /**< 是否自动映射 */
    net_prefix_t prefix_v4;              /**< 已配置 IPv4 前缀；addr.family=0 表示未配置 */
    net_prefix_t prefix_v6;              /**< 已配置 IPv6 前缀；addr.family=0 表示未配置 */
    int shutdown;                        /**< 1=shutdown（down），0=no shutdown（up） */
    int link_up;                         /**< -1=unknown，0=down，1=up（运行态，仅由监控事件更新） */
} if_map_entry_t;

/** 接口映射表（所有接口类型统一存放，key=逻辑名，value=if_map_entry_t*，按接口名有序） */
typedef struct
{
    GTree *all_entries; /**< 全部接口条目（GE、loop、null0）：key=g_strdup(logical_name)，有序红黑树 */
} if_map_t;

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
