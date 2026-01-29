#ifndef NN_DB_CLI_H
#define NN_DB_CLI_H

#include <stdint.h>

#include "nn_cfg.h"
#include "nn_dev.h"

// ============================================================================
// DB CLI Command Group IDs
// ============================================================================
#define NN_DB_CLI_GROUP_ID_SHOW_DB 1

typedef struct nn_db_cli_out
{
    uint32_t group_id;
    struct
    {
        char db_name[64];
        char table_name[64];
        int has_db_name;
        int has_table_name;
    } show_db;
} nn_db_cli_out_t;

/**
 * @brief Response output structure
 */
typedef struct nn_db_cli_resp_out
{
    char message[NN_CFG_CLI_MAX_RESP_LEN]; // Buffer for CLI output
    int success;
} nn_db_cli_resp_out_t;

/**
 * @brief Main entry point for DB CLI command processing
 * @param msg Message from cfg module containing CLI command
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_cli_process_command(nn_dev_message_t *msg);

#endif // NN_DB_CLI_H
