#ifndef NN_CLI_HANDLER_H
#define NN_CLI_HANDLER_H

#include <stdint.h>

#include "nn_cli_view.h"

#define MAX_CMD_LEN 1024
#define MAX_HOSTNAME_LEN 64

// Client session structure
typedef struct
{
    nn_cli_view_node_t *current_view; // Current view node
    char prompt[64];
} nn_cli_session_t;

// Function prototypes
void nn_cli_cleanup(void);
void handle_client(uint32_t client_fd);
void send_prompt(uint32_t client_fd, nn_cli_session_t *session);
void send_message(uint32_t client_fd, const char *message);
void process_command(uint32_t client_fd, const char *cmd_line, nn_cli_session_t *session);

// Built-in command handlers
void cmd_help(uint32_t client_fd, const char *args);
void cmd_exit(uint32_t client_fd, const char *args);
void cmd_show_version(uint32_t client_fd, const char *args);
void cmd_show_tree(uint32_t client_fd, const char *args);
void cmd_configure(uint32_t client_fd, const char *args);
void cmd_end(uint32_t client_fd, const char *args);
void cmd_sysname(uint32_t client_fd, const char *args);

// Hostname management
const char *nn_cli_get_hostname(void);
void nn_cli_set_hostname(const char *hostname);

#endif // NN_CLI_HANDLER_H
