#ifndef NN_CLI_HISTORY_H
#define NN_CLI_HISTORY_H

#include <stdint.h>
#include <time.h>

#define MAX_CMD_LEN 1024
#define MAX_CLIENT_IP_LEN 64
#define NN_CLI_SESSION_HISTORY_SIZE 20
#define NN_CLI_GLOBAL_HISTORY_SIZE 200

// History entry structure
typedef struct
{
    char *command;                     // Command string
    time_t timestamp;                  // Execution time
    char client_ip[MAX_CLIENT_IP_LEN]; // Client IP address
} nn_cli_history_entry_t;

// Session-specific history structure
typedef struct
{
    nn_cli_history_entry_t entries[NN_CLI_SESSION_HISTORY_SIZE];
    uint32_t count;
    uint32_t current_idx;
    int32_t browse_idx;            // Browse position (-1=current input, 0-19=history)
    char temp_buffer[MAX_CMD_LEN]; // Temporary save of current uncommitted input
} nn_cli_session_history_t;

// Global history structure
typedef struct
{
    nn_cli_history_entry_t entries[NN_CLI_GLOBAL_HISTORY_SIZE];
    uint32_t count;
    uint32_t current_idx;
} nn_cli_global_history_t;

// API for session history
void nn_cli_session_history_init(nn_cli_session_history_t *history);
void nn_cli_session_history_add(nn_cli_session_history_t *history, const char *cmd, const char *client_ip);
const char *nn_cli_session_history_get(nn_cli_session_history_t *history, uint32_t relative_idx);
const nn_cli_history_entry_t *nn_cli_session_history_get_entry(nn_cli_session_history_t *history,
                                                               uint32_t relative_idx);
void nn_cli_session_history_cleanup(nn_cli_session_history_t *history);

// API for global history
void nn_cli_global_history_init(nn_cli_global_history_t *history);
void nn_cli_global_history_add(nn_cli_global_history_t *history, const char *cmd, const char *client_ip);
const nn_cli_history_entry_t *nn_cli_global_history_get_entry(nn_cli_global_history_t *history, uint32_t relative_idx);
void nn_cli_global_history_cleanup(nn_cli_global_history_t *history);

#endif // NN_CLI_HISTORY_H
