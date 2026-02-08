/**
 * @file   db_cli.h
 * @brief  数据库模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef DB_CLI_H
#define DB_CLI_H

#include <stdint.h>

#include "cfg.h"
#include "dev.h"

// ============================================================================
// DB CLI Command Group IDs
// ============================================================================
#define DB_CLI_GROUP_ID_SHOW_DB 1
#define DB_CLI_SHOW_DB_CFG_ID_LIST 0x00000001
#define DB_CLI_SHOW_DB_CFG_ID_DB_NAME 0x00000002
#define DB_CLI_SHOW_DB_CFG_ID_TABLE_LIST 0x00000003
#define DB_CLI_SHOW_DB_CFG_ID_TABLE_FIELD 0x00000004
#define DB_CLI_SHOW_DB_CFG_ID_TABLE_DATA 0x00000005
#define DB_CLI_SHOW_DB_CFG_ID_TABLE_NAME 0x00000006

typedef struct
{
    char db_name[64];
    char table_name[64];
    gboolean is_db_list;
    gboolean is_table_data;
    gboolean is_table_field;
    gboolean is_table_list;
} show_db_t;

typedef struct db_cli_out
{
    uint32_t group_id;
    union
    {
        show_db_t show_db;
    } data;
} db_cli_out_t;

/**
 * @brief Response output structure
 */
typedef struct db_cli_resp_out
{
    char message[CFG_CLI_MAX_RESP_LEN]; // Buffer for CLI output
    int success;
    uint32_t has_more;     // 1 if more data available
    uint32_t batch_offset; // Continuation offset for next batch
} db_cli_resp_out_t;

/**
 * @brief Main entry point for DB CLI command processing
 * @param msg Message from cfg module containing CLI command
 * @return ERRCODE_SUCCESS or ERRCODE_FAIL
 */
int db_cli_process_command(ipc_message_t *msg);
int db_cli_handle_continue(ipc_message_t *msg);

#endif // DB_CLI_H
