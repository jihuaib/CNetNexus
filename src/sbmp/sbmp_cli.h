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
#define SBMP_CLI_GROUP_ID_SHOW 3        /**< show bmp server 命令 */

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

#endif /* SBMP_CLI_H */
