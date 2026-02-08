/**
 * @file   cfg_cli.h
 * @brief  CFG 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CFG_CLI_H
#define CFG_CLI_H

#include "cli_handler.h"
#include "cli_tree.h"

// ============================================================================
// Command Group and Element IDs
// ============================================================================

#define CFG_CLI_GROUP_ID_SHOW 0x00000001
#define CFG_CLI_SHOW_CFG_ID_COMMON_INFO 0x00000001
#define CFG_CLI_SHOW_CFG_ID_HISTORY 0x00000002
#define CFG_CLI_SHOW_CFG_ID_CURRENT_CONFIG 0x00000003

#define CFG_CLI_GROUP_ID_OP 0x00000002
#define CFG_CLI_OP_CFG_ID_EXIT 0x00000001
#define CFG_CLI_OP_CFG_ID_CONFIG 0x00000002
#define CFG_CLI_OP_CFG_ID_END 0x00000003

typedef struct cli_cfg_show
{
    gboolean is_common_info;
    gboolean is_history;
    gboolean is_current_config;
} cli_cfg_show_t;

typedef struct cli_cfg_op
{
    gboolean is_exit;
    gboolean is_end;
    gboolean is_config;
} cli_cfg_op_t;

typedef struct cfg_cli_out
{
    uint32_t group_id;
    union
    {
        cli_cfg_show_t cfg_show;
        cli_cfg_op_t cfg_op;
    } data;
} cfg_cli_out_t;

typedef struct cfg_cli_resp_out
{
    char message[CFG_CLI_MAX_RESP_LEN];
    int success;
    uint32_t has_more;     // 1 if more data available
    uint32_t batch_offset; // Continuation offset for next batch
} cfg_cli_resp_out_t;

int cfg_cli_handle(cli_match_result_t *result, cli_session_t *session);

#endif // CFG_CLI_H
