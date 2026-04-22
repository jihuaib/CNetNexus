/**
 * @file   if_cli.h
 * @brief  接口模块 CLI 配置命令处理（IPC 线程入口）
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
 * @brief 处理 CLI 配置命令（IPC 线程执行，内部通过 worker 同步派发 apply）
 * @param msg CLI 请求消息（内部不释放，由调用方负责）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int if_cli_handle_config_msg(dev_ipc_message_t *msg);

#endif // IF_CLI_H
