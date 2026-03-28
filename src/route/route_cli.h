/**
 * @file   route_cli.h
 * @brief  Route 模块 CLI 配置命令处理头文件（在 IPC 线程调用）
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_CLI_H
#define ROUTE_CLI_H

#include "dev.h"

/** Route CLI group_id 定义（与 commands.xml 中 group-id 一致） */
#define ROUTE_CLI_GROUP_ID_CONFIG 1      /**< 路由配置命令 */
#define ROUTE_CLI_GROUP_ID_SHOW 2        /**< show route 命令 */
#define ROUTE_CLI_GROUP_ID_BATCH 3       /**< 批量路由命令 */
#define ROUTE_CLI_GROUP_ID_RELAY_SHOW 4  /**< show route relay 命令 */
#define ROUTE_CLI_GROUP_ID_STATIC_SHOW 5 /**< show route static 命令（候选静态路由表） */

/**
 * @brief 处理来自 CFG 模块的 CLI 配置命令消息（在 IPC 线程调用）
 *
 * 解析 TLV 载荷，完成 DB 持久化后向 worker 线程发送配置应用命令并等待结果，
 * 最后通过 IPC 发送 CLI 响应。
 *
 * @param msg CLI 命令消息（由调用者保证非 NULL）
 * @return ERRCODE_SUCCESS 成功，ERRCODE_FAIL 失败
 */
int route_cli_handle_config_msg(dev_ipc_message_t *msg);

#endif /* ROUTE_CLI_H */
