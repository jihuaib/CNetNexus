#ifndef NN_CLI_HANDLER_H
#define NN_CLI_HANDLER_H

#include <stdint.h>
#include <time.h>

#include "nn_cli_history.h"
#include "nn_cli_view.h"

// Input state machine for ANSI escape sequences
typedef enum
{
    NN_CLI_STATE_NORMAL,
    NN_CLI_STATE_ESC,
    NN_CLI_STATE_CSI,
} nn_cli_input_state_t;

// Client session structure
typedef struct
{
    nn_cli_view_node_t *current_view; // Current view node
    char prompt[NN_CFG_CLI_MAX_PROMPT_LEN];
    nn_cli_session_history_t history;  // Command history
    char client_ip[MAX_CLIENT_IP_LEN]; // Client IP address
    int client_fd;
    char line_buffer[MAX_CMD_LEN]; // Current command buffer
    uint32_t line_pos;             // Current length of line_buffer
    uint32_t cursor_pos;           // Cursor position in buffer
    nn_cli_input_state_t state;    // Input state machine state

    // Tab completion cycling state
    uint32_t tab_cycling;            // 1 if currently cycling through matches
    uint32_t tab_match_index;        // Current index in tab matches
    char tab_original[MAX_CMD_LEN];  // Original input before tab cycling
    uint32_t tab_original_pos;       // Original cursor position before tab cycling
} nn_cli_session_t;

// Function prototypes
void nn_cli_cleanup(void);
nn_cli_session_t *nn_cli_session_create(int client_fd);
int nn_cli_process_input(nn_cli_session_t *session);
void nn_cli_session_destroy(nn_cli_session_t *session);
void send_prompt(nn_cli_session_t *session);
void update_prompt(nn_cli_session_t *session);
void update_prompt_from_template(nn_cli_session_t *session, const char *module_prompt);
void nn_cfg_send_message(nn_cli_session_t *session, const char *message);
void nn_cfg_send_data(nn_cli_session_t *session, const void *data, size_t len);
int process_command(const char *cmd_line, nn_cli_session_t *session);

#endif // NN_CLI_HANDLER_H
