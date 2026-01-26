#include "nn_cli_handler.h"

#include <ctype.h>
#include <dirent.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nn_cfg_cli.h"
#include "nn_cli_dispatch.h"
#include "nn_cli_param_type.h"
#include "nn_cli_tree.h"
#include "nn_cli_xml_parser.h"
#include "nn_dev.h"
#include "nn_errcode.h"

// Global CLI view tree
nn_cli_view_tree_t g_view_tree = {NULL, NULL};

// Global hostname
static char g_hostname[MAX_HOSTNAME_LEN] = "NetNexus";

// Get current hostname
const char *nn_cli_get_hostname(void)
{
    return g_hostname;
}

// Set new hostname
void nn_cli_set_hostname(const char *hostname)
{
    if (hostname && strlen(hostname) > 0)
    {
        strncpy(g_hostname, hostname, MAX_HOSTNAME_LEN - 1);
        g_hostname[MAX_HOSTNAME_LEN - 1] = '\0';
    }
}

// Send a message to the client (must be null-terminated)
void nn_cfg_send_message(uint32_t client_fd, const char *message)
{
    if (message)
    {
        write(client_fd, message, strlen(message));
    }
}

// Send raw data to the client with explicit length
void nn_cfg_send_data(uint32_t client_fd, const void *data, size_t len)
{
    if (data && len > 0)
    {
        write(client_fd, data, len);
    }
}

// Helper: Replace {hostname} in template and store in session prompt
static void replace_hostname_in_prompt(nn_cli_session_t *session, const char *template)
{
    char temp[128] = {0};
    const char *hostname_placeholder = "{hostname}";
    const char *pos = strstr(template, hostname_placeholder);

    if (pos)
    {
        size_t prefix_len = pos - template;
        size_t placeholder_len = strlen(hostname_placeholder);

        strncpy(temp, template, prefix_len);
        temp[prefix_len] = '\0';
        strncat(temp, nn_cli_get_hostname(), sizeof(temp) - strlen(temp) - 1);
        strncat(temp, pos + placeholder_len, sizeof(temp) - strlen(temp) - 1);
    }
    else
    {
        strncpy(temp, template, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    }

    strncpy(session->prompt, temp, sizeof(session->prompt) - 1);
    session->prompt[sizeof(session->prompt) - 1] = '\0';
}

// Update session prompt based on current view (for views without module-specific placeholders)
void update_prompt(nn_cli_session_t *session)
{
    if (!session || !session->current_view)
    {
        return;
    }

    replace_hostname_in_prompt(session, session->current_view->prompt_template);
}

// Update session prompt from module-filled template (module has already filled its placeholders)
void update_prompt_from_template(nn_cli_session_t *session, const char *module_prompt)
{
    if (!session || !module_prompt)
    {
        return;
    }

    // Module has filled its placeholders, we only need to replace {hostname}
    replace_hostname_in_prompt(session, module_prompt);
}

// Send the prompt to the client
void send_prompt(uint32_t client_fd, nn_cli_session_t *session)
{
    nn_cfg_send_message(client_fd, session->prompt);
    nn_cfg_send_message(client_fd, " ");
}

// Handle TAB key auto-completion
static void handle_tab_completion(uint32_t client_fd, nn_cli_session_t *session, char *line_buffer, uint32_t *line_pos)
{
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        return;
    }

    line_buffer[*line_pos] = '\0';

    // Get all matches for the last token
    nn_cli_tree_node_t *matches[50];
    uint32_t num_matches =
        nn_cli_tree_match_command_get_matches(session->current_view->cmd_tree, line_buffer, matches, 50);

    // Check if we have a trailing space
    uint32_t has_trailing_space = (*line_pos > 0 && line_buffer[*line_pos - 1] == ' ');

    if (num_matches == 1)
    {
        // Single match - show on new line
        nn_cli_tree_node_t *match = matches[0];

        nn_cfg_send_message(client_fd, "\r\n");

        // Redisplay prompt and current input
        send_prompt(client_fd, session);

        if (match->type == NN_CLI_NODE_COMMAND)
        {
            // KEYWORD: Auto-complete
            if (!has_trailing_space)
            {
                // Find the last token in line_buffer
                char *last_space = strrchr(line_buffer, ' ');
                char *last_token_start = last_space ? last_space + 1 : line_buffer;

                // Calculate new line position
                *line_pos = last_token_start - line_buffer;

                // Write the complete keyword
                const char *match_name = match->name;
                for (uint32_t i = 0; match_name[i] && *line_pos < MAX_CMD_LEN - 1; i++)
                {
                    line_buffer[*line_pos] = match_name[i];
                    (*line_pos)++;
                }

                // Add space after keyword
                if (*line_pos < MAX_CMD_LEN - 1)
                {
                    line_buffer[*line_pos] = ' ';
                    (*line_pos)++;
                }

                line_buffer[*line_pos] = '\0';
            }
        }
        else if (match->type == NN_CLI_NODE_ARGUMENT)
        {
            // ARGUMENT: Remove trailing space if exists
            if (has_trailing_space)
            {
                (*line_pos)--;
                line_buffer[*line_pos] = '\0';
            }
        }

        nn_cfg_send_message(client_fd, line_buffer);
    }
    else if (num_matches > 1)
    {
        // Multiple matches - Show all options on new lines
        nn_cfg_send_message(client_fd, "\r\n");
        for (uint32_t i = 0; i < num_matches; i++)
        {
            char option[256];
            char name_display[128];

            // Format name based on node type
            if (matches[i]->type == NN_CLI_NODE_ARGUMENT && matches[i]->param_type && matches[i]->param_type->type_str)
            {
                // ARGUMENT: Display as <type(range)>
                snprintf(name_display, sizeof(name_display), "<%s>", matches[i]->param_type->type_str);
            }
            else if (matches[i]->name)
            {
                // COMMAND or ARGUMENT without param_type: Display name as-is
                strncpy(name_display, matches[i]->name, sizeof(name_display) - 1);
                name_display[sizeof(name_display) - 1] = '\0';
            }
            else
            {
                // No name, skip
                continue;
            }

            snprintf(option, sizeof(option), "  %-25s - %s\r\n", name_display,
                     matches[i]->description ? matches[i]->description : "");
            nn_cfg_send_message(client_fd, option);
        }
        send_prompt(client_fd, session);
        if (has_trailing_space)
        {
            (*line_pos)--;
            line_buffer[*line_pos] = '\0';
        }
        nn_cfg_send_message(client_fd, line_buffer);
    }
    else
    {
        nn_cfg_send_message(client_fd, "\r\n");
        send_prompt(client_fd, session);
        nn_cfg_send_message(client_fd, line_buffer);
    }
}

// Handle ? key for help
static void handle_help_request(uint32_t client_fd, nn_cli_session_t *session, char *line_buffer, uint32_t line_pos)
{
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        return;
    }

    line_buffer[line_pos] = '\0';
    nn_cfg_send_message(client_fd, "\r\n");

    // Check if we have a trailing space
    uint32_t has_trailing_space = (line_pos > 0 && line_buffer[line_pos - 1] == ' ');
    char buffer[256];

    if (has_trailing_space)
    {
        // Case: "xx ?" - Show next token's children
        nn_cli_tree_node_t *context = nn_cli_tree_match_command(session->current_view->cmd_tree, line_buffer);

        if (context)
        {
            // Found valid context (could be root), show its children
            nn_cli_tree_print_help(context, client_fd);
        }
        else
        {
            // Command not found
            nn_cfg_send_message(client_fd, "Error: Invalid command.\r\n");
        }
    }
    else
    {
        // Case: "xx?" - Show matching keywords or argument help
        nn_cli_tree_node_t *matches[50];
        uint32_t num_matches =
            nn_cli_tree_match_command_get_matches(session->current_view->cmd_tree, line_buffer, matches, 50);

        if (num_matches > 0)
        {
            // Check if all matches are KEYWORD or if there's an ARGUMENT
            uint32_t has_keyword = 0;
            uint32_t has_argument = 0;

            for (uint32_t i = 0; i < num_matches; i++)
            {
                if (matches[i]->type == NN_CLI_NODE_COMMAND)
                {
                    has_keyword = 1;
                }
                else if (matches[i]->type == NN_CLI_NODE_ARGUMENT)
                {
                    has_argument = 1;
                }
            }

            if (has_keyword)
            {
                // Show all matching KEYWORD nodes
                for (uint32_t i = 0; i < num_matches; i++)
                {
                    if (matches[i]->type == NN_CLI_NODE_COMMAND && matches[i]->name && matches[i]->description)
                    {
                        snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", matches[i]->name, matches[i]->description);
                        nn_cfg_send_message(client_fd, buffer);
                    }
                }
                nn_cfg_send_message(client_fd, "\r\n");
            }
            else if (has_argument)
            {
                // Show ARGUMENT help (validation passed)
                nn_cli_tree_node_t *arg = matches[0];

                char name_display[128];
                if (arg->param_type && arg->param_type->type_str)
                {
                    snprintf(name_display, sizeof(name_display), "<%s>", arg->param_type->type_str);
                }
                else if (arg->name)
                {
                    strncpy(name_display, arg->name, sizeof(name_display) - 1);
                    name_display[sizeof(name_display) - 1] = '\0';
                }
                else
                {
                    strcpy(name_display, "<parameter>");
                }

                snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", name_display,
                         arg->description ? arg->description : "");
                nn_cfg_send_message(client_fd, buffer);
                nn_cfg_send_message(client_fd, "\r\n");
            }
        }
        else
        {
            // No matches found
            // Check if line_buffer is empty or only whitespace
            uint32_t is_empty = 1;
            for (uint32_t i = 0; i < line_pos; i++)
            {
                if (!isspace((unsigned char)line_buffer[i]))
                {
                    is_empty = 0;
                    break;
                }
            }

            if (is_empty)
            {
                // Empty command - show help for current context
                nn_cli_tree_node_t *context = session->current_view->cmd_tree;
                if (context)
                {
                    nn_cli_tree_print_help(context, client_fd);
                }
            }
            else
            {
                // Invalid command
                nn_cfg_send_message(client_fd, "Error: Invalid command.\r\n");
            }
        }
    }

    send_prompt(client_fd, session);
    nn_cfg_send_message(client_fd, line_buffer);
}

// Trim whitespace from both ends of a string
static char *trim(char *str)
{
    char *end;

    // Trim leading space
    while (isspace((unsigned char)*str))
    {
        str++;
    }

    if (*str == NN_ERRCODE_SUCCESS)
    {
        return str;
    }

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
    {
        end--;
    }

    // Write new null terminator
    *(end + 1) = '\0';

    return str;
}

// Help command handler
void cmd_help(uint32_t client_fd, const char *args)
{
    (void)args;
    nn_cfg_send_message(client_fd, "\r\nAvailable commands - use TAB for completion, ? for help\r\n");
}

// Exit command handler
void cmd_exit(uint32_t client_fd, const char *args)
{
    (void)args;
    nn_cfg_send_message(client_fd, "\r\nGoodbye!\r\n");
    close(client_fd);
}

// Show version command handler
void cmd_show_version(uint32_t client_fd, const char *args)
{
    (void)args;
    nn_cfg_send_message(client_fd, "\r\nNetNexus Telnet CLI Server\r\n");
    nn_cfg_send_message(client_fd, "Version: 1.0.0\r\n");
    nn_cfg_send_message(client_fd, "Build Date: " __DATE__ " " __TIME__ "\r\n");
    nn_cfg_send_message(client_fd, "\r\n");
}

// Forward declarations
static void print_view_commands_flat(nn_cli_view_node_t *view, uint32_t client_fd);
static void print_commands_recursive(uint32_t client_fd, uint32_t view_id, const char *prefix,
                                     nn_cli_tree_node_t *node);

// Process a command line
void process_command(uint32_t client_fd, const char *cmd_line, nn_cli_session_t *session)
{
    char buffer[MAX_CMD_LEN];
    strncpy(buffer, cmd_line, MAX_CMD_LEN - 1);
    buffer[MAX_CMD_LEN - 1] = '\0';

    char *trimmed = trim(buffer);

    // Empty command
    if (strlen(trimmed) == NN_ERRCODE_SUCCESS)
    {
        return;
    }

    // Get current view's command tree
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        nn_cfg_send_message(client_fd, "\r\nError: No command tree for current view\r\n");
        return;
    }

    // Use full match to get all element IDs and values
    nn_cli_match_result_t *match_result = nn_cli_tree_match_command_full(session->current_view->cmd_tree, trimmed);
    nn_cli_tree_node_t *node = match_result ? match_result->final_node : NULL;

    if (node)
    {
        // Check if command is complete (no required children)
        if (node->num_children > 0)
        {
            // Incomplete command - node has children that need to be specified
            nn_cfg_send_message(client_fd, "Error: Incomplete command.\r\n");

            // Free match result and return
            if (match_result)
            {
                nn_cli_match_result_free(match_result);
            }
            return;
        }

        // Dispatch to module if module_id is set
        if (match_result && match_result->module_id != 0)
        {
            if (match_result->module_id == NN_DEV_MODULE_ID_CFG)
            {
                nn_cfg_cli_handle(match_result, client_fd, session);
            }
            else
            {
                nn_cli_dispatch_to_module(match_result, client_fd, session);
            }
        }
    }
    else
    {
        nn_cfg_send_message(client_fd, "Error: Invalid command.\r\n");
    }

    // Free match result
    if (match_result)
    {
        nn_cli_match_result_free(match_result);
    }
}

// Cleanup CLI trees
void nn_cli_cleanup(void)
{
    if (g_view_tree.root)
    {
        nn_cli_view_free(g_view_tree.root);
        g_view_tree.root = NULL;
    }
    if (g_view_tree.global_view)
    {
        nn_cli_view_free(g_view_tree.global_view);
        g_view_tree.global_view = NULL;
    }
}

// Handle client connection
void handle_client(uint32_t client_fd)
{
    char line_buffer[MAX_CMD_LEN];
    uint32_t line_pos = 0;

    // Initialize session
    nn_cli_session_t session;
    session.current_view = g_view_tree.root;
    update_prompt(&session);

    // Enable telnet character mode
    unsigned char telnet_opts[] = {
        255, 251, 1,  // IAC WILL ECHO
        255, 251, 3,  // IAC WILL SUPPRESS_GO_AHEAD
        255, 253, 34, // IAC DO LINEMODE (with MODE 0 for character mode)
    };
    nn_cfg_send_data(client_fd, telnet_opts, sizeof(telnet_opts));

    // Send welcome message
    nn_cfg_send_message(client_fd, "\r\n");
    nn_cfg_send_message(client_fd, "Welcome to NetNexus CLI\r\n");
    nn_cfg_send_message(client_fd, "Type '?' for available commands\r\n");
    nn_cfg_send_message(client_fd, "\r\n");

    // Send initial prompt
    send_prompt(client_fd, &session);

    // Main input loop
    char c;
    while (read(client_fd, &c, 1) > 0)
    {
        // Filter out telnet protocol commands (IAC sequences)
        if ((unsigned char)c == 255)
        { // IAC
            // Read and discard the next 2 bytes (command and option)
            char discard[2];
            read(client_fd, discard, 2);
            continue;
        }

        // Handle Enter
        if (c == '\r' || c == '\n')
        {
            nn_cfg_send_message(client_fd, "\r\n");

            if (line_pos > 0)
            {
                line_buffer[line_pos] = '\0';

                // Process command
                process_command(client_fd, line_buffer, &session);

                // Reset line buffer
                line_pos = 0;
            }

            send_prompt(client_fd, &session);
        }
        // Handle Backspace
        else if (c == 127 || c == 8)
        {
            if (line_pos > 0)
            {
                line_pos--;
                nn_cfg_send_message(client_fd, "\b \b");
            }
        }
        // Handle TAB
        else if (c == '\t')
        {
            handle_tab_completion(client_fd, &session, line_buffer, &line_pos);
        }
        // Handle ?
        else if (c == '?')
        {
            handle_help_request(client_fd, &session, line_buffer, line_pos);
        }
        // Regular character
        else if (line_pos < MAX_CMD_LEN - 1 && c >= 32 && c < 127)
        {
            line_buffer[line_pos++] = c;
            nn_cfg_send_data(client_fd, &c, 1);
        }
    }
}

// Sysname command handler
void cmd_sysname(uint32_t client_fd, const char *args)
{
    if (!args || strlen(args) == NN_ERRCODE_SUCCESS)
    {
        // Show current hostname
        char msg[128];
        snprintf(msg, sizeof(msg), "\r\nCurrent hostname: %s\r\n", nn_cli_get_hostname());
        nn_cfg_send_message(client_fd, msg);
        return;
    }

    // Trim whitespace
    while (isspace((unsigned char)*args))
    {
        args++;
    }

    if (strlen(args) == NN_ERRCODE_SUCCESS)
    {
        nn_cfg_send_message(client_fd, "\r\nError: Hostname cannot be empty\r\n");
        return;
    }

    if (strlen(args) >= MAX_HOSTNAME_LEN)
    {
        nn_cfg_send_message(client_fd, "\r\nError: Hostname too long (max 63 characters)\r\n");
        return;
    }

    // Set new hostname
    nn_cli_set_hostname(args);

    char msg[128];
    snprintf(msg, sizeof(msg), "\r\nHostname set to: %s\r\n", nn_cli_get_hostname());
    nn_cfg_send_message(client_fd, msg);
}
