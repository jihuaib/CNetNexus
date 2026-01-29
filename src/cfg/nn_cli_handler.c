#include "nn_cli_handler.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nn_cfg_cli.h"
#include "nn_cfg_main.h"
#include "nn_cli_dispatch.h"
#include "nn_cli_param_type.h"
#include "nn_cli_tree.h"
#include "nn_cli_xml_parser.h"
#include "nn_dev.h"
#include "nn_errcode.h"

// Send a message to the client (must be null-terminated)
void nn_cfg_send_message(nn_cli_session_t *session, const char *message)
{
    if (message)
    {
        write(session->client_fd, message, strlen(message));
    }
}

// Send raw data to the client with explicit length
void nn_cfg_send_data(nn_cli_session_t *session, const void *data, size_t len)
{
    if (data && len > 0)
    {
        write(session->client_fd, data, len);
    }
}

// Update session prompt from module-filled template (module has already resolved placeholders like %u)
void update_prompt_from_template(nn_cli_session_t *session, const char *module_prompt)
{
    if (!session || !module_prompt)
    {
        return;
    }

    strncpy(session->prompt, module_prompt, sizeof(session->prompt) - 1);
    session->prompt[sizeof(session->prompt) - 1] = '\0';
}

// Push current prompt onto the stack (call before entering a sub-view)
void nn_cli_prompt_push(nn_cli_session_t *session)
{
    if (!session || session->prompt_stack_depth >= NN_CLI_PROMPT_STACK_DEPTH)
    {
        return;
    }

    strncpy(session->prompt_stack[session->prompt_stack_depth], session->prompt, NN_CFG_CLI_MAX_PROMPT_LEN - 1);
    session->prompt_stack[session->prompt_stack_depth][NN_CFG_CLI_MAX_PROMPT_LEN - 1] = '\0';
    session->prompt_stack_depth++;
}

// Pop prompt from the stack (call when exiting a sub-view)
void nn_cli_prompt_pop(nn_cli_session_t *session)
{
    if (!session || session->prompt_stack_depth == 0)
    {
        return;
    }

    session->prompt_stack_depth--;
    strncpy(session->prompt, session->prompt_stack[session->prompt_stack_depth], sizeof(session->prompt) - 1);
    session->prompt[sizeof(session->prompt) - 1] = '\0';
}

// Send the prompt to the client
void send_prompt(nn_cli_session_t *session)
{
    nn_cfg_send_message(session, session->prompt);
    nn_cfg_send_message(session, " ");
}

// ANSI escape sequences
#define ANSI_CLEAR_LINE "\x1B[2K"
#define ANSI_MOVE_START "\x1B[1G"
#define ANSI_CURSOR_LEFT "\x1B[D"
#define ANSI_CURSOR_RIGHT "\x1B[C"

// Redraw from cursor position to end of line
static void redraw_from_cursor(nn_cli_session_t *session, const char *buffer, uint32_t from_pos, uint32_t total_len)
{
    // Send characters from from_pos to end
    if (total_len > from_pos)
    {
        nn_cfg_send_data(session, buffer + from_pos, total_len - from_pos);
    }

    // Send a space to clear any old character
    nn_cfg_send_message(session, " ");

    // Move cursor back to correct position
    uint32_t move_back = total_len - from_pos + 1;
    for (uint32_t i = 0; i < move_back; i++)
    {
        nn_cfg_send_message(session, "\b");
    }
}

// Clear current line and redraw with new content
static void clear_and_redraw_line(nn_cli_session_t *session, const char *buffer, uint32_t len, uint32_t cursor_pos)
{
    // Move to start of line and clear it
    nn_cfg_send_message(session, "\r");
    nn_cfg_send_message(session, ANSI_CLEAR_LINE);

    // Redraw prompt
    send_prompt(session);

    // Redraw buffer content
    if (len > 0)
    {
        nn_cfg_send_data(session, buffer, len);
    }

    // Move cursor to correct position
    if (cursor_pos < len)
    {
        uint32_t move_back = len - cursor_pos;
        for (uint32_t i = 0; i < move_back; i++)
        {
            nn_cfg_send_message(session, "\b");
        }
    }
}

// Handle up arrow key - browse history backwards (newer to older)
static void handle_arrow_up(nn_cli_session_t *session, char *line_buffer, uint32_t *line_pos, uint32_t *cursor_pos)
{
    nn_cli_session_history_t *history = &session->history;

    if (history->count == 0)
    {
        return; // No history
    }

    // First time browsing? Save current input
    if (history->browse_idx == -1)
    {
        line_buffer[*line_pos] = '\0';
        strncpy(history->temp_buffer, line_buffer, MAX_CMD_LEN - 1);
        history->temp_buffer[MAX_CMD_LEN - 1] = '\0';

        // Load newest history (index 0)
        history->browse_idx = 0;
    }
    else if (history->browse_idx < (int32_t)(history->count - 1))
    {
        // Continue browsing backwards
        history->browse_idx++;
    }
    else
    {
        return; // Already at oldest
    }

    // Get history command
    const char *hist_cmd = nn_cli_session_history_get(history, history->browse_idx);
    if (hist_cmd)
    {
        strncpy(line_buffer, hist_cmd, MAX_CMD_LEN - 1);
        line_buffer[MAX_CMD_LEN - 1] = '\0';
        *line_pos = strlen(line_buffer);
        *cursor_pos = *line_pos;

        clear_and_redraw_line(session, line_buffer, *line_pos, *cursor_pos);
    }
}

// Handle down arrow key - browse history forwards (older to newer)
static void handle_arrow_down(nn_cli_session_t *session, char *line_buffer, uint32_t *line_pos, uint32_t *cursor_pos)
{
    nn_cli_session_history_t *history = &session->history;

    if (history->browse_idx == -1)
    {
        return; // Not browsing, ignore
    }

    if (history->browse_idx > 0)
    {
        // Continue browsing forwards
        history->browse_idx--;

        const char *hist_cmd = nn_cli_session_history_get(history, history->browse_idx);
        if (hist_cmd)
        {
            strncpy(line_buffer, hist_cmd, MAX_CMD_LEN - 1);
            line_buffer[MAX_CMD_LEN - 1] = '\0';
            *line_pos = strlen(line_buffer);
            *cursor_pos = *line_pos;

            clear_and_redraw_line(session, line_buffer, *line_pos, *cursor_pos);
        }
    }
    else
    {
        // Back to current input (restore temp_buffer)
        history->browse_idx = -1;

        strncpy(line_buffer, history->temp_buffer, MAX_CMD_LEN - 1);
        line_buffer[MAX_CMD_LEN - 1] = '\0';
        *line_pos = strlen(line_buffer);
        *cursor_pos = *line_pos;

        clear_and_redraw_line(session, line_buffer, *line_pos, *cursor_pos);
    }
}

// Handle left arrow key - move cursor left
static void handle_arrow_left(nn_cli_session_t *session, uint32_t *cursor_pos)
{
    if (*cursor_pos > 0)
    {
        (*cursor_pos)--;
        nn_cfg_send_message(session, "\b");
    }
}

// Handle right arrow key - move cursor right
static void handle_arrow_right(nn_cli_session_t *session, uint32_t line_pos, uint32_t *cursor_pos)
{
    if (*cursor_pos < line_pos)
    {
        nn_cfg_send_message(session, ANSI_CURSOR_RIGHT);
        (*cursor_pos)++;
    }
}

// Print help for a node
static void nn_cli_tree_print_help(nn_cli_tree_node_t *node, nn_cli_session_t *session)
{
    if (!node)
    {
        return;
    }

    char buffer[512];

    // If current node is an end node, show <cr> option first
    if (node->is_end_node)
    {
        snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", "<cr>", "Execute command");
        nn_cfg_send_message(session, buffer);
    }

    for (uint32_t i = 0; i < node->num_children; i++)
    {
        nn_cli_tree_node_t *child = node->children[i];
        if (child->description)
        {
            char name_display[128];
            char desc_with_marker[256];

            if (child->type == NN_CLI_NODE_ARGUMENT && child->param_type && child->param_type->type_str)
            {
                // ARGUMENT: Display as <type(range)>
                snprintf(name_display, sizeof(name_display), "<%s>", child->param_type->type_str);
            }
            else if (child->name)
            {
                // COMMAND or ARGUMENT without param_type: Display name as-is
                strncpy(name_display, child->name, sizeof(name_display) - 1);
                name_display[sizeof(name_display) - 1] = '\0';
            }
            else
            {
                // No name, skip this child
                continue;
            }

            // Add marker if child is also an end node
            if (child->is_end_node)
            {
                snprintf(desc_with_marker, sizeof(desc_with_marker), "%s", child->description);
            }
            else
            {
                strncpy(desc_with_marker, child->description, sizeof(desc_with_marker) - 1);
                desc_with_marker[sizeof(desc_with_marker) - 1] = '\0';
            }

            snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", name_display, desc_with_marker);
            nn_cfg_send_message(session, buffer);
        }
    }
}

// Apply a tab completion match to the line buffer
static void tab_apply_match(nn_cli_tree_node_t *match, char *line_buffer, uint32_t *line_pos,
                            uint32_t has_trailing_space)
{
    if (match->type == NN_CLI_NODE_COMMAND)
    {
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
        if (has_trailing_space)
        {
            (*line_pos)--;
            line_buffer[*line_pos] = '\0';
        }
    }
}

// Handle TAB key auto-completion (cycles through matches on repeated TAB presses)
static void handle_tab_completion(nn_cli_session_t *session, char *line_buffer, uint32_t *line_pos)
{
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        return;
    }

    // If already cycling, use original input for matching
    char match_input[MAX_CMD_LEN];
    if (session->tab_cycling)
    {
        memcpy(match_input, session->tab_original, session->tab_original_pos);
        match_input[session->tab_original_pos] = '\0';
    }
    else
    {
        line_buffer[*line_pos] = '\0';
        memcpy(match_input, line_buffer, *line_pos + 1);
    }

    // Get all matches for the last token
    nn_cli_tree_node_t *matches[50];
    uint32_t num_matches =
        nn_cli_tree_match_command_get_matches(session->current_view->cmd_tree, match_input, matches, 50);

    // Check if we have a trailing space in the original input
    uint32_t orig_len = session->tab_cycling ? session->tab_original_pos : *line_pos;
    uint32_t has_trailing_space = (orig_len > 0 && match_input[orig_len - 1] == ' ');

    if (num_matches == 1)
    {
        // Single match - auto-complete directly
        session->tab_cycling = 0;
        nn_cli_tree_node_t *match = matches[0];

        nn_cfg_send_message(session, "\r\n");
        send_prompt(session);

        tab_apply_match(match, line_buffer, line_pos, has_trailing_space);
        nn_cfg_send_message(session, line_buffer);
    }
    else if (num_matches > 1)
    {
        // Multiple matches - cycle through them one by one
        if (!session->tab_cycling)
        {
            // First TAB press: save original state and start cycling
            session->tab_cycling = 1;
            session->tab_match_index = 0;
            memcpy(session->tab_original, line_buffer, *line_pos);
            session->tab_original_pos = *line_pos;
        }
        else
        {
            // Subsequent TAB press: advance to next match
            session->tab_match_index = (session->tab_match_index + 1) % num_matches;
        }

        nn_cli_tree_node_t *match = matches[session->tab_match_index];

        // Restore original input before applying new match
        memcpy(line_buffer, session->tab_original, session->tab_original_pos);
        *line_pos = session->tab_original_pos;
        line_buffer[*line_pos] = '\0';

        nn_cfg_send_message(session, "\r\n");
        send_prompt(session);

        tab_apply_match(match, line_buffer, line_pos, has_trailing_space);
        nn_cfg_send_message(session, line_buffer);
    }
    else
    {
        session->tab_cycling = 0;
        nn_cfg_send_message(session, "\r\n");
        send_prompt(session);
        nn_cfg_send_message(session, line_buffer);
    }
}

// Handle ? key for help
static void handle_help_request(nn_cli_session_t *session, char *line_buffer, uint32_t *line_pos, uint32_t cursor_pos)
{
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        return;
    }

    nn_cfg_send_message(session, "\r\n");

    // Create temp buffer with only [0, cursor_pos) for matching
    char match_buffer[MAX_CMD_LEN];
    memcpy(match_buffer, line_buffer, cursor_pos);
    match_buffer[cursor_pos] = '\0';

    // Check if we have a trailing space in the cursor range
    uint32_t has_trailing_space = (cursor_pos > 0 && match_buffer[cursor_pos - 1] == ' ');
    char buffer[256];

    if (has_trailing_space)
    {
        // Case: "xx ?" - Show next token's children
        nn_cli_tree_node_t *context = nn_cli_tree_match_command(session->current_view->cmd_tree, match_buffer);

        if (context)
        {
            // Found valid context (could be root), show its children
            nn_cli_tree_print_help(context, session);
        }
        else
        {
            // Command not found
            nn_cfg_send_message(session, "Error: Invalid command.\r\n");
        }
    }
    else
    {
        // Case: "xx?" - Show matching keywords or argument help
        nn_cli_tree_node_t *matches[50];
        uint32_t num_matches =
            nn_cli_tree_match_command_get_matches(session->current_view->cmd_tree, match_buffer, matches, 50);

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
                        nn_cfg_send_message(session, buffer);
                    }
                }
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
                nn_cfg_send_message(session, buffer);
            }
        }
        else
        {
            // No matches found
            // Check if match_buffer is empty or only whitespace
            uint32_t is_empty = 1;
            for (uint32_t i = 0; i < cursor_pos; i++)
            {
                if (!isspace((unsigned char)match_buffer[i]))
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
                    nn_cli_tree_print_help(context, session);
                }
            }
            else
            {
                // Invalid command
                nn_cfg_send_message(session, "Error: Invalid command.\r\n");
            }
        }
    }

    // Truncate line_buffer at cursor position
    *line_pos = cursor_pos;
    line_buffer[*line_pos] = '\0';

    // Clear current line and redraw with proper cursor position
    nn_cfg_send_message(session, "\r");      // Move to start of line
    nn_cfg_send_message(session, "\x1B[2K"); // Clear entire line

    send_prompt(session);
    if (*line_pos > 0)
    {
        nn_cfg_send_data(session, line_buffer, *line_pos);
    }
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

// Process a command line
// Returns: 1 if command executed successfully, 0 if error
int process_command(const char *cmd_line, nn_cli_session_t *session)
{
    char buffer[MAX_CMD_LEN];
    strncpy(buffer, cmd_line, MAX_CMD_LEN - 1);
    buffer[MAX_CMD_LEN - 1] = '\0';

    char *trimmed = trim(buffer);

    // Empty command
    if (strlen(trimmed) == NN_ERRCODE_SUCCESS)
    {
        return 0; // Empty command, don't record
    }

    // Get current view's command tree
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        nn_cfg_send_message(session, "\r\nError: No command tree for current view\r\n");
        return 0; // Error
    }

    // Use full match to get all element IDs and values
    nn_cli_match_result_t *match_result = nn_cli_tree_match_command_full(session->current_view->cmd_tree, trimmed);
    nn_cli_tree_node_t *node = match_result ? match_result->final_node : NULL;

    if (node)
    {
        // Check if command is complete (node must be marked as an end node)
        if (!node->is_end_node)
        {
            // Incomplete command - node is not a valid command end point
            nn_cfg_send_message(session, "Error: Incomplete command.\r\n");

            // Free match result and return
            if (match_result)
            {
                nn_cli_match_result_free(match_result);
            }
            return 0; // Error
        }

        // Dispatch to module if module_id is set
        if (match_result && match_result->module_id != 0)
        {
            if (match_result->module_id == NN_DEV_MODULE_ID_CFG)
            {
                nn_cfg_cli_handle(match_result, session);
            }
            else
            {
                nn_cli_dispatch_to_module(match_result, session);
            }
        }

        // Free match result
        if (match_result)
        {
            nn_cli_match_result_free(match_result);
        }
        return 1; // Success
    }
    else
    {
        nn_cfg_send_message(session, "Error: Invalid command.\r\n");

        // Free match result
        if (match_result)
        {
            nn_cli_match_result_free(match_result);
        }
        return 0; // Error
    }
}

// Cleanup CLI trees
void nn_cli_cleanup(void)
{
    if (g_nn_cfg_local == NULL)
    {
        return;
    }
    if (g_nn_cfg_local->view_tree.root)
    {
        nn_cli_view_free(g_nn_cfg_local->view_tree.root);
        g_nn_cfg_local->view_tree.root = NULL;
    }
    if (g_nn_cfg_local->view_tree.global_view)
    {
        nn_cli_view_free(g_nn_cfg_local->view_tree.global_view);
        g_nn_cfg_local->view_tree.global_view = NULL;
    }
}

// Create a new client session
nn_cli_session_t *nn_cli_session_create(int client_fd)
{
    nn_cli_session_t *session = g_malloc0(sizeof(nn_cli_session_t));
    if (!session)
    {
        return NULL;
    }

    // Set non-blocking mode
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    session->client_fd = client_fd;
    session->current_view = g_nn_cfg_local->view_tree.root;
    session->line_pos = 0;
    session->cursor_pos = 0;
    session->state = NN_CLI_STATE_NORMAL;

    update_prompt_from_template(session, session->current_view->prompt_template);
    nn_cli_session_history_init(&session->history);

    // Get client IP address
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    if (getpeername(client_fd, (struct sockaddr *)&client_addr, &addr_len) == 0)
    {
        inet_ntop(AF_INET, &client_addr.sin_addr, session->client_ip, sizeof(session->client_ip));
    }
    else
    {
        strcpy(session->client_ip, "unknown");
    }

    // Enable telnet character mode
    unsigned char telnet_opts[] = {
        255, 251, 1,  // IAC WILL ECHO
        255, 251, 3,  // IAC WILL SUPPRESS_GO_AHEAD
        255, 253, 34, // IAC DO LINEMODE (with MODE 0 for character mode)
    };
    nn_cfg_send_data(session, telnet_opts, sizeof(telnet_opts));

    // Send welcome message
    nn_cfg_send_message(session, "\r\n");
    nn_cfg_send_message(session, "Welcome to NetNexus CLI\r\n");
    nn_cfg_send_message(session, "Type '?' for available commands\r\n");
    nn_cfg_send_message(session, "\r\n");

    // Send initial prompt
    send_prompt(session);

    return session;
}

// Process available input for a session
// Returns: 0 on success, -1 on disconnect/error
int nn_cli_process_input(nn_cli_session_t *session)
{
    char c;
    ssize_t n;

    while ((n = read(session->client_fd, &c, 1)) > 0)
    {
        // Filter out telnet protocol commands (IAC sequences)
        if ((unsigned char)c == 255)
        { // IAC
            // Read and discard the next 2 bytes (command and option)
            char discard[2];
            if (read(session->client_fd, discard, 2) < 0)
            {
                // This shouldn't happen usually but for non-blocking...
                // In character mode, telnet opts should arrive together.
            }
            continue;
        }

        // State machine for ANSI escape sequences
        if (session->state == NN_CLI_STATE_NORMAL)
        {
            // Check for ESC key (start of escape sequence)
            if (c == 27)
            {
                session->state = NN_CLI_STATE_ESC;
                continue;
            }

            // Reset tab cycling state on any non-tab input
            if (c != '\t')
            {
                session->tab_cycling = 0;
            }

            // Handle Enter
            if (c == '\r' || c == '\n')
            {
                nn_cfg_send_message(session, "\r\n");

                if (session->line_pos > 0)
                {
                    session->line_buffer[session->line_pos] = '\0';

                    // Process command and add to history only if successful
                    int cmd_success = process_command(session->line_buffer, session);
                    // Add to local session history
                    nn_cli_session_history_add(&session->history, session->line_buffer, session->client_ip);
                    if (cmd_success)
                    {
                        // Add to global history (thread-safe)
                        pthread_mutex_lock(&g_nn_cfg_local->history_mutex);
                        nn_cli_global_history_add(&g_nn_cfg_local->global_history, session->line_buffer,
                                                  session->client_ip);
                        pthread_mutex_unlock(&g_nn_cfg_local->history_mutex);
                    }

                    // Reset line buffer
                    session->line_pos = 0;
                    session->cursor_pos = 0;
                    session->history.browse_idx = -1; // Reset browse state
                }

                send_prompt(session);
            }
            // Handle Backspace
            else if (c == 127 || c == 8)
            {
                if (session->cursor_pos > 0)
                {
                    if (session->cursor_pos < session->line_pos)
                    {
                        // Delete in middle: shift characters left
                        memmove(session->line_buffer + session->cursor_pos - 1,
                                session->line_buffer + session->cursor_pos, session->line_pos - session->cursor_pos);
                        session->line_pos--;
                        session->cursor_pos--;
                        // Redraw from cursor to end
                        nn_cfg_send_message(session, "\b");
                        redraw_from_cursor(session, session->line_buffer, session->cursor_pos, session->line_pos);
                    }
                    else
                    {
                        // Delete at end (original logic)
                        session->line_pos--;
                        session->cursor_pos--;
                        nn_cfg_send_message(session, "\b \b");
                    }
                }
            }
            // Handle TAB
            else if (c == '\t')
            {
                // Use only [0, cursor_pos) for completion
                char temp[MAX_CMD_LEN];
                memcpy(temp, session->line_buffer, session->cursor_pos);
                temp[session->cursor_pos] = '\0';

                uint32_t old_cursor = session->cursor_pos;
                handle_tab_completion(session, temp, &session->cursor_pos);

                // If completion modified the buffer, update line_buffer
                if (session->cursor_pos != old_cursor || strcmp(temp, session->line_buffer) != 0)
                {
                    // Copy completed content back
                    memcpy(session->line_buffer, temp, session->cursor_pos);
                    session->line_pos = session->cursor_pos;
                    // Note: handle_tab_completion already redraws the line
                }
            }
            // Handle ?
            else if (c == '?')
            {
                // Pass full line_buffer but handle_help_request will only use [0, cursor_pos) for matching
                handle_help_request(session, session->line_buffer, &session->line_pos, session->cursor_pos);
            }
            // Regular character
            else if (session->line_pos < MAX_CMD_LEN - 1 && c >= 32 && c < 127)
            {
                if (session->cursor_pos < session->line_pos)
                {
                    // Insert in middle: shift characters right
                    memmove(session->line_buffer + session->cursor_pos + 1, session->line_buffer + session->cursor_pos,
                            session->line_pos - session->cursor_pos);
                    session->line_buffer[session->cursor_pos] = c;
                    session->line_pos++;
                    session->cursor_pos++;
                    // Redraw from cursor-1 to end
                    redraw_from_cursor(session, session->line_buffer, session->cursor_pos - 1, session->line_pos);
                }
                else
                {
                    // Append at end (original logic)
                    session->line_buffer[session->line_pos++] = c;
                    session->cursor_pos++;
                    nn_cfg_send_data(session, &c, 1);
                }
            }
        }
        else if (session->state == NN_CLI_STATE_ESC)
        {
            // After ESC, expect '[' for CSI sequence
            if (c == '[')
            {
                session->state = NN_CLI_STATE_CSI;
            }
            else
            {
                // Not a CSI sequence, ignore and reset
                session->state = NN_CLI_STATE_NORMAL;
            }
        }
        else if (session->state == NN_CLI_STATE_CSI)
        {
            // Handle arrow keys
            if (c == 'A')
            {
                // Up arrow
                handle_arrow_up(session, session->line_buffer, &session->line_pos, &session->cursor_pos);
                session->state = NN_CLI_STATE_NORMAL;
            }
            else if (c == 'B')
            {
                // Down arrow
                handle_arrow_down(session, session->line_buffer, &session->line_pos, &session->cursor_pos);
                session->state = NN_CLI_STATE_NORMAL;
            }
            else if (c == 'C')
            {
                // Right arrow
                handle_arrow_right(session, session->line_pos, &session->cursor_pos);
                session->state = NN_CLI_STATE_NORMAL;
            }
            else if (c == 'D')
            {
                // Left arrow
                handle_arrow_left(session, &session->cursor_pos);
                session->state = NN_CLI_STATE_NORMAL;
            }
            else
            {
                // Unknown CSI sequence, ignore
                session->state = NN_CLI_STATE_NORMAL;
            }
        }
    }

    if (n == 0)
    {
        // Disconnected
        return -1;
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        // Error
        return -1;
    }

    return 0;
}

// Destroy a client session
void nn_cli_session_destroy(nn_cli_session_t *session)
{
    if (!session)
    {
        return;
    }

    nn_cli_session_history_cleanup(&session->history);
    if (session->client_fd >= 0)
    {
        close(session->client_fd);
    }
    g_free(session);
}
