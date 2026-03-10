/**
 * @file   cli_dispatch.h
 * @brief  CLI 命令分发头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_DISPATCH_H
#define CLI_DISPATCH_H

#include <stdint.h>

#include "cli_handler.h"
#include "cli_tree.h"

/**
 * @brief 分发命令到目标模块
 * @param result 命令匹配结果
 * @param session CLI 会话
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int cli_dispatch_to_module(cli_match_result_t *result, cli_session_t *session);

#endif // CLI_DISPATCH_H
