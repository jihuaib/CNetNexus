/**
 * @file   dev.h
 * @brief  设备模块公共接口，定义模块 ID 和模块注册 API
 * @author jhb
 * @date   2026/01/22
 */

#ifndef DEV_H
#define DEV_H

#include <stdint.h>

// ============================================================================
// 模块 ID 定义
// ============================================================================

/** DEV 模块 */
#define DEV_MODULE_ID_DEV 0x00000001
/** DB 模块 */
#define DEV_MODULE_ID_DB 0x00000002
/** CFG 模块 */
#define DEV_MODULE_ID_CFG 0x00000003
/** IF 接口模块 */
#define DEV_MODULE_ID_IF 0x00000004
/** BGP 模块 */
#define DEV_MODULE_ID_BGP 0x00000005
/** Route 模块 */
#define DEV_MODULE_ID_ROUTE 0x00000006

/** 无效文件描述符 */
#define DEV_INVALID_FD (-1)

/** 模块名称最大长度 */
#define DEV_MODULE_NAME_MAX_LEN 12

// ============================================================================
// 前向声明
// ============================================================================

typedef struct ipc_context ipc_context_t;
typedef struct ipc_message ipc_message_t;

/**
 * @brief IPC 消息处理回调函数类型
 * @param ctx IPC 上下文
 * @param msg 接收到的消息
 */
typedef void (*ipc_msg_handler_fn)(ipc_context_t *ctx, ipc_message_t *msg);

// ============================================================================
// 公共 API
// ============================================================================

/**
 * @brief 通过 DEV RPC 根据模块 ID 获取模块名称
 * @param ctx 调用方 IPC 上下文
 * @param module_id 模块 ID
 * @param module_name 输出模块名称缓冲区
 * @return 成功返回 0，失败返回 -1
 */
int dev_get_module_name(ipc_context_t *ctx, uint32_t module_id, char *module_name);

#endif // DEV_H
