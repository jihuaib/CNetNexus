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
#define ROUTE_CLI_GROUP_ID_CONFIG 1 /**< 路由配置命令 */
#define ROUTE_CLI_GROUP_ID_SHOW 2   /**< show route 命令 */

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

#endif // ROUTE_CLI_H
