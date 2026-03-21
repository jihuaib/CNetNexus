/**
 * @file   route_cli.h
 * @brief  Route 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_CLI_H
#define ROUTE_CLI_H

#include "cli.h"
#include "dev.h"

/** Route CLI group_id 定义（与 commands.xml 中 group-id 一致） */
#define ROUTE_CLI_GROUP_ID_CONFIG 1     /**< 路由配置命令 */
#define ROUTE_CLI_GROUP_ID_SHOW 2       /**< show route 命令 */
#define ROUTE_CLI_GROUP_ID_BATCH 3      /**< 批量路由命令 */
#define ROUTE_CLI_GROUP_ID_RELAY_SHOW 4 /**< show route relay 命令 */

/**
 * @brief 处理来自 CFG 模块的 CLI 命令消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int route_cli_handle_message(dev_ipc_message_t *msg);

/**
 * @brief 处理 CLI continue 消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int route_cli_handle_continue(dev_ipc_message_t *msg);

/**
 * @brief 处理 show current-configuration 请求
 * @param msg 消息
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int route_cli_handle_show_config(dev_ipc_message_t *msg);

/**
 * @brief 清理 Route CLI 内部状态（如分片输出缓存）
 */
void route_cli_cleanup_state(void);

/**
 * @brief 从 DB route_batch 表恢复所有 batch 路由到内存 RIB（Phase 3 调用，走统一加路由通知入口）
 * @param ctx IPC 上下文
 */
void route_batch_restore_from_db(dev_ipc_context_t *ctx);

#endif /* ROUTE_CLI_H */
