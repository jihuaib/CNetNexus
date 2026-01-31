/**
 * @file   nn_dev_cli.h
 * @brief  Dev 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef NN_DEV_CLI_H
#define NN_DEV_CLI_H

#include "nn_cfg.h"
#include "nn_dev.h"

// Command group IDs from commands.xml
#define NN_DEV_CLI_GROUP_ID_SHOW_VERSION 1
#define NN_DEV_CLI_GROUP_ID_SYSNAME 2
#define NN_DEV_CLI_GROUP_ID_SHOW_MODULE 3
#define NN_DEV_CLI_GROUP_ID_SHOW_MODULE_MQ 4

typedef struct nn_dev_cli_out
{
    uint32_t group_id;
    // For now, dev commands don't carry complex output data
} nn_dev_cli_out_t;

/**
 * @brief Response output structure
 */
typedef struct nn_dev_cli_resp_out
{
    char message[NN_CFG_CLI_MAX_RESP_LEN]; // Buffer for CLI output
    int success;
    uint32_t has_more;                     // 1 if more data available
    uint32_t batch_offset;                 // Continuation offset for next batch
} nn_dev_cli_resp_out_t;

int nn_dev_cli_handle_message(nn_dev_message_t *msg);
int nn_dev_cli_handle_continue(nn_dev_message_t *msg);

#endif // NN_DEV_CLI_H
