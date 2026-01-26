//
// Created by j31397 on 2026/1/26.
//

#ifndef NETNEXUS_NN_CFG_CLI_H
#define NETNEXUS_NN_CFG_CLI_H

#include <stdbool.h>

#include "nn_cli_handler.h"
#include "nn_cli_tree.h"

// ============================================================================
// Command Group and Element IDs
// ============================================================================

#define NN_CFG_CLI_GROUP_ID_SHOW_CLI 0x00000001

#define NN_CFG_CLI_GROUP_ID_OP 0x00000002
#define NN_CFG_CLI_OP_ELEM_ID_EXIT 0x00000001
#define NN_CFG_CLI_OP_ELEM_ID_CONFIG 0x00000002
#define NN_CFG_CLI_OP_ELEM_ID_END 0x00000003

typedef struct nn_cli_cfg_op
{
    bool is_exit;
    bool is_end;
    bool is_config;
} nn_cli_cfg_op_t;

typedef struct nn_cfg_cli_out
{
    uint32_t group_id;
    union
    {
        nn_cli_cfg_op_t cfg_op;
    } data;
} nn_cfg_cli_out_t;

typedef struct nn_cfg_cli_resp_out
{
    char message[256];
    int success;
} nn_cfg_cli_resp_out_t;

int nn_cfg_cli_handle(nn_cli_match_result_t *result, uint32_t client_fd, nn_cli_session_t *session);

#endif // NETNEXUS_NN_CFG_CLI_H
