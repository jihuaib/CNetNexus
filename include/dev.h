/**
 * @file   dev.h
 * @brief  公共接口，定义模块 ID 和通用工具函数
 * @author jhb
 * @date   2026/01/22
 *
 * @note   消息结构已迁移到 ipc.h 中的 ipc_message_t
 */

#ifndef DEV_H
#define DEV_H

#include <stdint.h>

// ============================================================================
// 模块 ID 定义
// ============================================================================

/** DEV 模块（已废弃，保留 ID 兼容） */
#define DEV_MODULE_ID_DEV 0x00000001
/** DB 模块 */
#define DEV_MODULE_ID_DB 0x00000002
/** CLI 模块 */
#define DEV_MODULE_ID_CLI 0x00000003
/** @deprecated 使用 DEV_MODULE_ID_CLI */
#define DEV_MODULE_ID_CFG DEV_MODULE_ID_CLI
/** IF 接口模块 */
#define DEV_MODULE_ID_IF 0x00000004
/** BGP 模块 */
#define DEV_MODULE_ID_BGP 0x00000005
/** ROUTE 路由模块 */
#define DEV_MODULE_ID_ROUTE 0x00000006
/** ACCESS 接入模块 */
#define DEV_MODULE_ID_ACCESS 0x00000007

/** 无效文件描述符 */
#define DEV_INVALID_FD (-1)

/** 模块名称最大长度 */
#define DEV_MODULE_NAME_MAX_LEN 12

/**
 * @brief 根据模块 ID 获取模块名称
 * @param module_id 模块 ID
 * @param module_name 输出模块名称缓冲区（至少 DEV_MODULE_NAME_MAX_LEN 字节）
 * @return 成功返回 0，失败返回 -1
 */
int dev_get_module_name(uint32_t module_id, char *module_name);

#endif // DEV_H
