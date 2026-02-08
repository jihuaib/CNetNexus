/**
 * @file   ipc_config.h
 * @brief  IPC 配置文件解析头文件
 * @author jhb
 * @date   2026/02/02
 */

#ifndef IPC_CONFIG_H
#define IPC_CONFIG_H

#include <stdint.h>

/** 单个模块的地址配置 */
typedef struct ipc_module_addr
{
    uint32_t module_id;   /**< 模块 ID */
    char name[32];        /**< 模块名称 */
    char host[64];        /**< 主机地址 */
    uint16_t port;        /**< 端口号 */
} ipc_module_addr_t;

/** IPC 配置，包含所有模块地址 */
typedef struct ipc_config
{
    ipc_module_addr_t modules[16]; /**< 模块地址数组 */
    int num_modules;                  /**< 模块数量 */
} ipc_config_t;

/**
 * @brief 加载 IPC 配置文件
 * @param path 配置文件路径
 * @param config 输出配置结构
 * @return 成功返回 0，失败返回 -1
 */
int ipc_config_load(const char *path, ipc_config_t *config);

/**
 * @brief 根据模块 ID 查找地址配置
 * @param config 配置结构
 * @param module_id 模块 ID
 * @return 地址配置指针，未找到返回 NULL
 */
const ipc_module_addr_t *ipc_config_find(const ipc_config_t *config, uint32_t module_id);

/**
 * @brief 解析 IPC 配置文件路径（自动查找）
 * @param config_path 用户指定路径（可为 NULL）
 * @param out_path 输出路径缓冲区
 * @param out_size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int ipc_resolve_config_path(const char *config_path, char *out_path, int out_size);

#endif // IPC_CONFIG_H
