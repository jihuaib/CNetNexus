/**
 * @file   cli_dispatch.h
 * @brief  CLI 命令分发头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_DISPATCH_H
#define CLI_DISPATCH_H

#include <stdint.h>

#include "cli_tree.h"

// 消息载荷有两种格式：
//
// 1. 旧 TLV 格式（无 db/table 关联的命令使用）:
//    [group_id(4B)] [cfg_id(4B) + len(2B) + value]...
//
// 2. 新 DB table 格式（关联了 db/table 的命令使用）:
//    [flags(1B)] [db_name(string)] [table_name(string)]
//    [num_fields(2B)] [field_flags(1B) + field_name(string) + value_type(1B) + value]...

#include "cli_handler.h"

// 打包旧 TLV 格式（兼容无 db/table 关联的命令）
uint8_t *cli_dispatch_pack_tlv(cli_match_result_t *result, uint32_t *out_len);

// 分发命令到目标模块
int cli_dispatch_to_module(cli_match_result_t *result, cli_session_t *session);

#endif // CLI_DISPATCH_H
