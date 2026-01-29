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
} nn_dev_cli_resp_out_t;

int nn_dev_cli_handle_message(nn_dev_message_t *msg);

#endif // NN_DEV_CLI_H
