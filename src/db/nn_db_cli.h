#ifndef NN_DB_CLI_H
#define NN_DB_CLI_H

#include <stdint.h>

#include "nn_cfg.h"
#include "nn_dev.h"

// ============================================================================
// DB CLI Command Group IDs
// ============================================================================
#define NN_DB_CLI_GROUP_ID_SHOW_DB 1
#define NN_DB_CLI_SHOW_DB_CFG_ID_LIST 0x00000001
#define NN_DB_CLI_SHOW_DB_CFG_ID_DB_NAME 0x00000002
#define NN_DB_CLI_SHOW_DB_CFG_ID_TABLE_LIST 0x00000003
#define NN_DB_CLI_SHOW_DB_CFG_ID_TABLE_FIELD 0x00000004
#define NN_DB_CLI_SHOW_DB_CFG_ID_TABLE_DATA 0x00000005
#define NN_DB_CLI_SHOW_DB_CFG_ID_TABLE_NAME 0x00000006

typedef struct
{
    char db_name[64];
    char table_name[64];
    gboolean is_db_list;
    gboolean is_table_data;
    gboolean is_table_field;
    gboolean is_table_list;
} show_db_t;

typedef struct nn_db_cli_out
{
    uint32_t group_id;
    union
    {
        show_db_t show_db;
    } data;
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
