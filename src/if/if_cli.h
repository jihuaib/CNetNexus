/**
 * @file   if_cli.h
 * @brief  接口模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef IF_CLI_H
#define IF_CLI_H

#include "cli.h"
#include "dev.h"

/** IF CLI group_id 定义（与 commands.xml 中 group-id 一致） */
#define IF_CLI_GROUP_ID_ENTRY 1  /**< 接口视图进入命令 */
#define IF_CLI_GROUP_ID_CONFIG 2 /**< 接口配置命令（ip address / shutdown） */
#define IF_CLI_GROUP_ID_SHOW 3   /**< show interface 命令 */

/**
 * @brief 处理来自 CFG 模块的 CLI 命令消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int if_cli_handle_message(ipc_message_t *msg);

/**
 * @brief 处理 CLI continue 消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int if_cli_handle_continue(ipc_message_t *msg);

#endif // IF_CLI_H
