/**
 * @file   cfg_cli.h
 * @brief  CFG 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CFG_CLI_H
#define CFG_CLI_H

#include "cli.h"
#include "cli_handler.h"
#include "cli_tree.h"

/** CFG CLI group_id 定义（与 commands.xml 中 group-id 一致） */
#define CFG_CLI_GROUP_ID_SHOW_COMMANDS 1 /**< show cli command-info */
#define CFG_CLI_GROUP_ID_SHOW_HISTORY 2  /**< show cli history */
#define CFG_CLI_GROUP_ID_SHOW_CONFIG 3   /**< show current-configuration */
#define CFG_CLI_GROUP_ID_EXIT 4          /**< exit */
#define CFG_CLI_GROUP_ID_CONFIG 5        /**< config */
#define CFG_CLI_GROUP_ID_END 6           /**< end */
#define CFG_CLI_GROUP_ID_BASH 7          /**< bash */

/**
 * @brief 响应输出结构
 */
typedef struct cfg_cli_resp_out
{
    char message[CLI_MAX_RESP_LEN];
    int success;
    uint32_t has_more;     /**< 是否有更多数据 */
    uint32_t batch_offset; /**< 续传偏移量 */
} cfg_cli_resp_out_t;

/**
 * @brief CFG 模块本地 CLI 命令处理
 * @param msg 已打包的 DB payload 消息
 * @param session CLI 会话
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int cfg_cli_handle(dev_ipc_message_t *msg, cli_session_t *session);

#endif // CFG_CLI_H
