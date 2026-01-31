/**
 * @file   nn_cfg_cli.h
 * @brief  CFG 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef NN_CFG_CLI_H
#define NN_CFG_CLI_H

#include "nn_cli_handler.h"
#include "nn_cli_tree.h"

// ============================================================================
// Command Group and Element IDs
// ============================================================================

#define NN_CFG_CLI_GROUP_ID_SHOW 0x00000001
#define NN_CFG_CLI_SHOW_CFG_ID_COMMON_INFO 0x00000001
#define NN_CFG_CLI_SHOW_CFG_ID_HISTORY 0x00000002
#define NN_CFG_CLI_SHOW_CFG_ID_CURRENT_CONFIG 0x00000003

#define NN_CFG_CLI_GROUP_ID_OP 0x00000002
#define NN_CFG_CLI_OP_CFG_ID_EXIT 0x00000001
#define NN_CFG_CLI_OP_CFG_ID_CONFIG 0x00000002
#define NN_CFG_CLI_OP_CFG_ID_END 0x00000003

typedef struct nn_cli_cfg_show
{
    gboolean is_common_info;
    gboolean is_history;
    gboolean is_current_config;
} nn_cli_cfg_show_t;

typedef struct nn_cli_cfg_op
{
    gboolean is_exit;
    gboolean is_end;
    gboolean is_config;
} nn_cli_cfg_op_t;

typedef struct nn_cfg_cli_out
{
    uint32_t group_id;
    union
    {
        nn_cli_cfg_show_t cfg_show;
        nn_cli_cfg_op_t cfg_op;
    } data;
} nn_cfg_cli_out_t;

typedef struct nn_cfg_cli_resp_out
{
    char message[NN_CFG_CLI_MAX_RESP_LEN];
    int success;
    uint32_t has_more;     // 1 if more data available
    uint32_t batch_offset; // Continuation offset for next batch
} nn_cfg_cli_resp_out_t;

int nn_cfg_cli_handle(nn_cli_match_result_t *result, nn_cli_session_t *session);

#endif // NN_CFG_CLI_H
