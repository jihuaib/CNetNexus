/**
 * @file   vrf_cli.h
 * @brief  VRF 模块 CLI 命令处理声明
 * @author jhb
 * @date   2026/03/05
 */
#ifndef VRF_CLI_H
#define VRF_CLI_H

#include "dev.h"

/**
 * @brief 处理 CLI 命令消息（CFG_MSG_TYPE_CLI）
 * @param msg 原始 IPC 消息
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int vrf_cli_handle_message(dev_ipc_message_t *msg);

/**
 * @brief 处理 CLI 分块继续请求（CFG_MSG_TYPE_CLI_CONTINUE）
 * @param msg 原始 IPC 消息
 * @return 成功返回 ERRCODE_SUCCESS
 */
int vrf_cli_handle_continue(dev_ipc_message_t *msg);

#endif /* VRF_CLI_H */
