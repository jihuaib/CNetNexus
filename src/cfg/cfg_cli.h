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
int cfg_cli_handle(ipc_message_t *msg, cli_session_t *session);

#endif // CFG_CLI_H
