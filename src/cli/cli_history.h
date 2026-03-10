/**
 * @file   cli_history.h
 * @brief  CLI 命令历史管理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_HISTORY_H
#define CLI_HISTORY_H

#include <stdint.h>
#include <time.h>

#define MAX_CMD_LEN 1024
#define MAX_CLIENT_IP_LEN 64
#define CLI_SESSION_HISTORY_SIZE 20
#define CLI_GLOBAL_HISTORY_SIZE 200

// History entry structure
typedef struct
{
    char *command;                     // Command string
    time_t timestamp;                  // Execution time
    char client_ip[MAX_CLIENT_IP_LEN]; // Client IP address
} cli_history_entry_t;

// Session-specific history structure
typedef struct
{
    cli_history_entry_t entries[CLI_SESSION_HISTORY_SIZE];
    uint32_t count;
    uint32_t current_idx;
    int32_t browse_idx;            // Browse position (-1=current input, 0-19=history)
    char temp_buffer[MAX_CMD_LEN]; // Temporary save of current uncommitted input
} cli_session_history_t;

// Global history structure
typedef struct
{
    cli_history_entry_t entries[CLI_GLOBAL_HISTORY_SIZE];
    uint32_t count;
    uint32_t current_idx;
} cli_global_history_t;

// API for session history
void cli_session_history_init(cli_session_history_t *history);
void cli_session_history_add(cli_session_history_t *history, const char *cmd, const char *client_ip);
const char *cli_session_history_get(cli_session_history_t *history, uint32_t relative_idx);
const cli_history_entry_t *cli_session_history_get_entry(cli_session_history_t *history, uint32_t relative_idx);
void cli_session_history_cleanup(cli_session_history_t *history);

// API for global history
void cli_global_history_init(cli_global_history_t *history);
void cli_global_history_add(cli_global_history_t *history, const char *cmd, const char *client_ip);
const cli_history_entry_t *cli_global_history_get_entry(cli_global_history_t *history, uint32_t relative_idx);
void cli_global_history_cleanup(cli_global_history_t *history);

#endif // CLI_HISTORY_H
