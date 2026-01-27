#ifndef NN_DB_CLI_H
#define NN_DB_CLI_H

#include <stdint.h>

#include "nn_cfg.h"
#include "nn_dev.h"

// ============================================================================
// DB CLI Command Group IDs
// ============================================================================
#define NN_DB_CLI_GROUP_ID_SHOW_DB 1

// ============================================================================
// DB CLI Command Handler
// ============================================================================

/**
 * @brief Main entry point for DB CLI command processing
 * @param msg Message from cfg module containing CLI command
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_cli_process_command(nn_dev_message_t *msg);

#endif // NN_DB_CLI_H
