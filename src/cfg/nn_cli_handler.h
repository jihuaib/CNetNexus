/**
 * @file   nn_cli_handler.h
 * @brief  CLI 客户端会话管理头文件
 * @author jhb
 * @date   2026/01/22
 */
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

#define NN_CLI_PROMPT_STACK_DEPTH 8

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
    uint32_t tab_cycling;           // 1 if currently cycling through matches
    uint32_t tab_match_index;       // Current index in tab matches
    char tab_original[MAX_CMD_LEN]; // Original input before tab cycling
    uint32_t tab_original_pos;      // Original cursor position before tab cycling

    // Prompt stack: saves prompt before entering sub-views
    char prompt_stack[NN_CLI_PROMPT_STACK_DEPTH][NN_CFG_CLI_MAX_PROMPT_LEN];
    uint32_t prompt_stack_depth;

    // 视图上下文栈：保存进入子视图时模块设置的环境变量
    uint8_t *view_context_stack[NN_CLI_PROMPT_STACK_DEPTH]; // 每层上下文 TLV 数据
    uint32_t view_context_len[NN_CLI_PROMPT_STACK_DEPTH];   // 每层数据长度

    // Pager state for --More-- output
    char *pager_buffer;              // Dynamically allocated output buffer
    uint32_t pager_offset;           // Current position in buffer
    uint32_t pager_total_len;        // Total buffer length
    uint32_t pager_lines_per_page;   // Lines per screen (default 24)
    uint32_t pager_active;           // 1 if pager is active
} nn_cli_session_t;

// Function prototypes
void nn_cli_cleanup(void);
nn_cli_session_t *nn_cli_session_create(int client_fd);
int nn_cli_process_input(nn_cli_session_t *session);
void nn_cli_session_destroy(nn_cli_session_t *session);
void send_prompt(nn_cli_session_t *session);
void update_prompt(nn_cli_session_t *session);
void update_prompt_from_template(nn_cli_session_t *session, const char *module_prompt);
void nn_cli_prompt_push(nn_cli_session_t *session);
void nn_cli_prompt_pop(nn_cli_session_t *session);
void nn_cli_context_set(nn_cli_session_t *session, const uint8_t *data, uint32_t len);
const uint8_t *nn_cli_context_get(nn_cli_session_t *session, uint32_t *out_len);
void nn_cfg_send_message(nn_cli_session_t *session, const char *message);
void nn_cfg_send_data(nn_cli_session_t *session, const void *data, size_t len);
int process_command(const char *cmd_line, nn_cli_session_t *session);
void nn_cli_pager_output(nn_cli_session_t *session, const char *message);
void nn_cli_pager_stop(nn_cli_session_t *session);

#endif // NN_CLI_HANDLER_H
