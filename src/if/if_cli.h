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
#define IF_CLI_GROUP_ID_ENTRY 1      /**< GE 接口视图进入命令 */
#define IF_CLI_GROUP_ID_CONFIG 2     /**< GE/loop 接口 ip address / shutdown 命令 */
#define IF_CLI_GROUP_ID_SHOW 3       /**< show interface 命令 */
#define IF_CLI_GROUP_ID_LOOP_ENTRY 4 /**< loop 接口进入/删除命令 */

/**
 * @brief 处理来自 CFG 模块的 CLI 命令消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int if_cli_handle_message(dev_ipc_message_t *msg);

/**
 * @brief 处理 CLI continue 消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int if_cli_handle_continue(dev_ipc_message_t *msg);

/**
 * @brief 清理 IF CLI 分片状态
 */
void if_cli_cleanup_state(void);

/**
 * @brief 通过 IF CLI 分片流发送文本（供 show 与 show current-configuration 共用）
 * @param msg 原始请求消息
 * @param full_text 完整文本（函数接管所有权，可为 NULL）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int if_cli_send_chunked_response(dev_ipc_message_t *msg, GString *full_text);

#endif // IF_CLI_H
