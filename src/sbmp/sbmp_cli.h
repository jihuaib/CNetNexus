/**
 * @file   sbmp_cli.h
 * @brief  SBMP 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/03/08
 */
#ifndef SBMP_CLI_H
#define SBMP_CLI_H

#include "cli.h"
#include "dev.h"

/** SBMP CLI group_id 定义（与 commands.xml 中 group-id 一致） */
#define SBMP_CLI_GROUP_ID_BMP_SERVER 1  /**< bmp-server 进入视图命令 */
#define SBMP_CLI_GROUP_ID_SERVER_PORT 2 /**< server port 配置命令 */
#define SBMP_CLI_GROUP_ID_SHOW_STATUS 3 /**< show bmp-server */
#define SBMP_CLI_GROUP_ID_SHOW_CLIENT 4 /**< show bmp-server client [id] */
#define SBMP_CLI_GROUP_ID_SHOW_PEER 5   /**< show bmp-server peer ... */
#define SBMP_CLI_GROUP_ID_SHOW_ROUTE 6  /**< show bmp-server route ... */

/**
 * @brief 处理来自 CFG 模块的 CLI 命令消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int sbmp_cli_handle_message(dev_ipc_message_t *msg);

/**
 * @brief 处理 CLI continue 消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int sbmp_cli_handle_continue(dev_ipc_message_t *msg);

/**
 * @brief 清理 SBMP CLI 分片状态
 */
void sbmp_cli_cleanup_state(void);

/**
 * @brief 通过 SBMP CLI 分片流发送文本（供 show 与 show current-configuration 共用）
 * @param msg 原始请求消息
 * @param full_text 完整文本（函数接管所有权，可为 NULL）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int sbmp_cli_send_chunked_response(dev_ipc_message_t *msg, GString *full_text);

#endif /* SBMP_CLI_H */
