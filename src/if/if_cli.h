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
