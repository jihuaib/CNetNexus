/**
 * @file   cli_handler.c
 * @brief  CLI 客户端会话管理，命令输入处理和执行
 * @author jhb
 * @date   2026/01/22
 */
#include "cli_handler.h"

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

#include "cfg_cli.h"
#include "cli_dispatch.h"
#include "cli_engine.h"
#include "cli_param_type.h"
#include "cli_tree.h"
#include "cli_xml_parser.h"
#include "dev.h"
#include "errcode.h"

// Send a message to the client (must be null-terminated)
void cfg_send_message(cli_session_t *session, const char *message)
{
    if (message)
    {
        write(session->client_fd, message, strlen(message));
    }
}

// Send raw data to the client with explicit length
void cfg_send_data(cli_session_t *session, const void *data, size_t len)
{
    if (data && len > 0)
    {
        write(session->client_fd, data, len);
    }
}

// Update session prompt from module-filled template (module has already resolved placeholders like %u)
void update_prompt_from_template(cli_session_t *session, const char *module_prompt)
{
    if (!session || !module_prompt)
    {
        return;
    }

    strncpy(session->prompt, module_prompt, sizeof(session->prompt) - 1);
    session->prompt[sizeof(session->prompt) - 1] = '\0';
}

// Push current prompt onto the stack (call before entering a sub-view)
void cli_prompt_push(cli_session_t *session)
{
    if (!session || session->prompt_stack_depth >= CLI_PROMPT_STACK_DEPTH)
    {
        return;
    }

    strncpy(session->prompt_stack[session->prompt_stack_depth], session->prompt, CFG_CLI_MAX_PROMPT_LEN - 1);
    session->prompt_stack[session->prompt_stack_depth][CFG_CLI_MAX_PROMPT_LEN - 1] = '\0';

    // 初始化当前层上下文为空（上下文在 VIEW_CHG 处理时设置）
    session->view_context_stack[session->prompt_stack_depth] = NULL;
    session->view_context_len[session->prompt_stack_depth] = 0;

    session->prompt_stack_depth++;
}

// Pop prompt from the stack (call when exiting a sub-view)
void cli_prompt_pop(cli_session_t *session)
{
    if (!session || session->prompt_stack_depth == 0)
    {
        return;
    }

    session->prompt_stack_depth--;

    // 释放当前层上下文数据
    if (session->view_context_stack[session->prompt_stack_depth])
    {
        g_free(session->view_context_stack[session->prompt_stack_depth]);
        session->view_context_stack[session->prompt_stack_depth] = NULL;
        session->view_context_len[session->prompt_stack_depth] = 0;
    }

    strncpy(session->prompt, session->prompt_stack[session->prompt_stack_depth], sizeof(session->prompt) - 1);
    session->prompt[sizeof(session->prompt) - 1] = '\0';
}

// 设置当前层视图上下文数据
void cli_context_set(cli_session_t *session, const uint8_t *data, uint32_t len)
{
    if (!session || session->prompt_stack_depth == 0 || !data || len == 0)
    {
        return;
    }

    // 上下文保存在当前层（prompt_stack_depth - 1，即刚 push 的那一层）
    uint32_t idx = session->prompt_stack_depth - 1;

    // 释放旧数据
    if (session->view_context_stack[idx])
    {
        g_free(session->view_context_stack[idx]);
    }

    session->view_context_stack[idx] = g_malloc(len);
    memcpy(session->view_context_stack[idx], data, len);
    session->view_context_len[idx] = len;
}

// 获取当前层视图上下文数据
const uint8_t *cli_context_get(cli_session_t *session, uint32_t *out_len)
{
    if (!session || !out_len)
    {
        if (out_len)
        {
            *out_len = 0;
        }
        return NULL;
    }

    if (session->prompt_stack_depth == 0)
    {
        *out_len = 0;
        return NULL;
    }

    uint32_t idx = session->prompt_stack_depth - 1;
    *out_len = session->view_context_len[idx];
    return session->view_context_stack[idx];
}

// Send the prompt to the client
void send_prompt(cli_session_t *session)
{
    cfg_send_message(session, session->prompt);
    cfg_send_message(session, " ");
}

// ============================================================================
// Pager (--More--) implementation
// ============================================================================

#define CLI_PAGER_DEFAULT_LINES 24
#define CLI_PAGER_PROMPT "--More--"

// Count the number of lines in a string (count \n occurrences)
static uint32_t pager_count_lines(const char *text)
{
    uint32_t count = 0;
    for (const char *p = text; *p; p++)
    {
        if (*p == '\n')
        {
            count++;
        }
    }
    return count;
}

// Display the --More-- prompt
static void pager_show_more_prompt(cli_session_t *session)
{
    cfg_send_message(session, CLI_PAGER_PROMPT);
}

// Clear the --More-- prompt from the line
static void pager_clear_more_prompt(cli_session_t *session)
{
    // \r moves to start, spaces overwrite, \r moves back to start
    cfg_send_message(session, "\r        \r");
}

// Show next N lines from pager buffer. Returns number of lines shown.
static uint32_t pager_send_lines(cli_session_t *session, uint32_t max_lines)
{
    if (!session->pager_buffer || session->pager_offset >= session->pager_total_len)
    {
        return 0;
    }

    uint32_t lines_sent = 0;
    uint32_t start = session->pager_offset;
    uint32_t pos = start;

    while (pos < session->pager_total_len && lines_sent < max_lines)
    {
        if (session->pager_buffer[pos] == '\n')
        {
            lines_sent++;
        }
        pos++;
    }

    // Send the data from start to pos
    if (pos > start)
    {
        cfg_send_data(session, session->pager_buffer + start, pos - start);
    }

    session->pager_offset = pos;
    return lines_sent;
}

// Start paged output. If content is short enough, send directly.
void cli_pager_output(cli_session_t *session, const char *message)
{
    if (!session || !message || message[0] == '\0')
    {
        return;
    }

    uint32_t lines = pager_count_lines(message);
    uint32_t page_size = session->pager_lines_per_page > 0 ? session->pager_lines_per_page : CLI_PAGER_DEFAULT_LINES;

    // If output fits on one screen, send directly
    if (lines <= page_size)
    {
        cfg_send_message(session, message);
        return;
    }

    // Start paging: copy message to pager buffer
    uint32_t msg_len = strlen(message);
    session->pager_buffer = g_malloc(msg_len + 1);
    memcpy(session->pager_buffer, message, msg_len + 1);
    session->pager_total_len = msg_len;
    session->pager_offset = 0;
    session->pager_active = 1;

    // Show first page
    pager_send_lines(session, page_size);

    // If there's more content, show --More-- prompt
    if (session->pager_offset < session->pager_total_len)
    {
        pager_show_more_prompt(session);
    }
    else
    {
        cli_pager_stop(session);
    }
}

// Stop pager and clean up
void cli_pager_stop(cli_session_t *session)
{
    if (!session)
    {
        return;
    }

    if (session->pager_active)
    {
        pager_clear_more_prompt(session);
    }

    if (session->pager_buffer)
    {
        g_free(session->pager_buffer);
        session->pager_buffer = NULL;
    }
    session->pager_offset = 0;
    session->pager_total_len = 0;
    session->pager_active = 0;
}

// Handle pager keypress. Returns 1 if key was consumed by pager, 0 otherwise.
static int pager_handle_key(cli_session_t *session, char c)
{
    if (!session->pager_active)
    {
        return 0;
    }

    uint32_t page_size = session->pager_lines_per_page > 0 ? session->pager_lines_per_page : CLI_PAGER_DEFAULT_LINES;

    if (c == ' ')
    {
        // Next page
        pager_clear_more_prompt(session);
        pager_send_lines(session, page_size);

        if (session->pager_offset < session->pager_total_len)
        {
            pager_show_more_prompt(session);
        }
        else
        {
            cli_pager_stop(session);
            send_prompt(session);
        }
    }
    else if (c == '\r' || c == '\n')
    {
        // Next line
        pager_clear_more_prompt(session);
        pager_send_lines(session, 1);

        if (session->pager_offset < session->pager_total_len)
        {
            pager_show_more_prompt(session);
        }
        else
        {
            cli_pager_stop(session);
            send_prompt(session);
        }
    }
    else if (c == 'q' || c == 'Q')
    {
        // Quit pager
        cli_pager_stop(session);
        send_prompt(session);
    }

    // All other keys are ignored while pager is active
    return 1;
}

// ANSI escape sequences
#define ANSI_CLEAR_LINE "\x1B[2K"
#define ANSI_MOVE_START "\x1B[1G"
#define ANSI_CURSOR_LEFT "\x1B[D"
#define ANSI_CURSOR_RIGHT "\x1B[C"

// Redraw from cursor position to end of line
static void redraw_from_cursor(cli_session_t *session, const char *buffer, uint32_t from_pos, uint32_t total_len)
{
    // Send characters from from_pos to end
    if (total_len > from_pos)
    {
        cfg_send_data(session, buffer + from_pos, total_len - from_pos);
    }

    // Send a space to clear any old character
    cfg_send_message(session, " ");

    // Move cursor back to correct position
    uint32_t move_back = total_len - from_pos + 1;
    for (uint32_t i = 0; i < move_back; i++)
    {
        cfg_send_message(session, "\b");
    }
}

// Clear current line and redraw with new content
static void clear_and_redraw_line(cli_session_t *session, const char *buffer, uint32_t len, uint32_t cursor_pos)
{
    // Move to start of line and clear it
    cfg_send_message(session, "\r");
    cfg_send_message(session, ANSI_CLEAR_LINE);

    // Redraw prompt
    send_prompt(session);

    // Redraw buffer content
    if (len > 0)
    {
        cfg_send_data(session, buffer, len);
    }

    // Move cursor to correct position
    if (cursor_pos < len)
    {
        uint32_t move_back = len - cursor_pos;
        for (uint32_t i = 0; i < move_back; i++)
        {
            cfg_send_message(session, "\b");
        }
    }
}

// Handle up arrow key - browse history backwards (newer to older)
static void handle_arrow_up(cli_session_t *session, char *line_buffer, uint32_t *line_pos, uint32_t *cursor_pos)
{
    cli_session_history_t *history = &session->history;

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
    const char *hist_cmd = cli_session_history_get(history, history->browse_idx);
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
static void handle_arrow_down(cli_session_t *session, char *line_buffer, uint32_t *line_pos, uint32_t *cursor_pos)
{
    cli_session_history_t *history = &session->history;

    if (history->browse_idx == -1)
    {
        return; // Not browsing, ignore
    }

    if (history->browse_idx > 0)
    {
        // Continue browsing forwards
        history->browse_idx--;

        const char *hist_cmd = cli_session_history_get(history, history->browse_idx);
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
static void handle_arrow_left(cli_session_t *session, uint32_t *cursor_pos)
{
    if (*cursor_pos > 0)
    {
        (*cursor_pos)--;
        cfg_send_message(session, "\b");
    }
}

// Handle right arrow key - move cursor right
static void handle_arrow_right(cli_session_t *session, uint32_t line_pos, uint32_t *cursor_pos)
{
    if (*cursor_pos < line_pos)
    {
        cfg_send_message(session, ANSI_CURSOR_RIGHT);
        (*cursor_pos)++;
    }
}

// Print help for a node into a GString buffer
static void cli_tree_print_help(cli_tree_node_t *node, GString *out)
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
        g_string_append(out, buffer);
    }

    for (uint32_t i = 0; i < node->num_children; i++)
    {
        cli_tree_node_t *child = node->children[i];
        if (child->description)
        {
            char name_display[128];
            char desc_with_marker[256];

            if (child->type == CLI_NODE_ARGUMENT && child->param_type && child->param_type->type_str)
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
            g_string_append(out, buffer);
        }
    }
}

// Apply a tab completion match to the line buffer
static void tab_apply_match(cli_tree_node_t *match, char *line_buffer, uint32_t *line_pos,
                            uint32_t has_trailing_space)
{
    if (match->type == CLI_NODE_COMMAND)
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
    else if (match->type == CLI_NODE_ARGUMENT)
    {
        if (has_trailing_space)
        {
            (*line_pos)--;
            line_buffer[*line_pos] = '\0';
        }
    }
}

// Handle TAB key auto-completion (cycles through matches on repeated TAB presses)
static void handle_tab_completion(cli_session_t *session, char *line_buffer, uint32_t *line_pos)
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
    cli_tree_node_t *matches[50];
    uint32_t num_matches =
        cli_tree_match_command_get_matches(session->current_view->cmd_tree, match_input, matches, 50);

    // Check if we have a trailing space in the original input
    uint32_t orig_len = session->tab_cycling ? session->tab_original_pos : *line_pos;
    uint32_t has_trailing_space = (orig_len > 0 && match_input[orig_len - 1] == ' ');

    if (num_matches == 1)
    {
        // Single match - auto-complete directly
        session->tab_cycling = 0;
        cli_tree_node_t *match = matches[0];

        cfg_send_message(session, "\r\n");
        send_prompt(session);

        tab_apply_match(match, line_buffer, line_pos, has_trailing_space);
        cfg_send_message(session, line_buffer);
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

        cli_tree_node_t *match = matches[session->tab_match_index];

        // Restore original input before applying new match
        memcpy(line_buffer, session->tab_original, session->tab_original_pos);
        *line_pos = session->tab_original_pos;
        line_buffer[*line_pos] = '\0';

        cfg_send_message(session, "\r\n");
        send_prompt(session);

        tab_apply_match(match, line_buffer, line_pos, has_trailing_space);
        cfg_send_message(session, line_buffer);
    }
    else
    {
        session->tab_cycling = 0;
        cfg_send_message(session, "\r\n");
        send_prompt(session);
        cfg_send_message(session, line_buffer);
    }
}

// Handle ? key for help
static void handle_help_request(cli_session_t *session, char *line_buffer, uint32_t *line_pos, uint32_t cursor_pos)
{
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        return;
    }

    cfg_send_message(session, "\r\n");

    // Create temp buffer with only [0, cursor_pos) for matching
    char match_buffer[MAX_CMD_LEN];
    memcpy(match_buffer, line_buffer, cursor_pos);
    match_buffer[cursor_pos] = '\0';

    // Check if we have a trailing space in the cursor range
    uint32_t has_trailing_space = (cursor_pos > 0 && match_buffer[cursor_pos - 1] == ' ');
    char buffer[256];

    // Collect all help output into a GString for paging
    GString *help_out = g_string_new("");

    if (has_trailing_space)
    {
        // Case: "xx ?" - Show next token's children
        cli_tree_node_t *context = cli_tree_match_command(session->current_view->cmd_tree, match_buffer);

        if (context)
        {
            cli_tree_print_help(context, help_out);
        }
        else
        {
            g_string_append(help_out, "Error: Invalid command.\r\n");
        }
    }
    else
    {
        // Case: "xx?" - Show matching keywords or argument help
        cli_tree_node_t *matches[50];
        uint32_t num_matches =
            cli_tree_match_command_get_matches(session->current_view->cmd_tree, match_buffer, matches, 50);

        if (num_matches > 0)
        {
            uint32_t has_keyword = 0;
            uint32_t has_argument = 0;

            for (uint32_t i = 0; i < num_matches; i++)
            {
                if (matches[i]->type == CLI_NODE_COMMAND)
                {
                    has_keyword = 1;
                }
                else if (matches[i]->type == CLI_NODE_ARGUMENT)
                {
                    has_argument = 1;
                }
            }

            if (has_keyword)
            {
                for (uint32_t i = 0; i < num_matches; i++)
                {
                    if (matches[i]->type == CLI_NODE_COMMAND && matches[i]->name && matches[i]->description)
                    {
                        snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", matches[i]->name, matches[i]->description);
                        g_string_append(help_out, buffer);
                    }
                }
            }
            else if (has_argument)
            {
                cli_tree_node_t *arg = matches[0];

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
                g_string_append(help_out, buffer);
            }
        }
        else
        {
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
                cli_tree_node_t *context = session->current_view->cmd_tree;
                if (context)
                {
                    cli_tree_print_help(context, help_out);
                }
            }
            else
            {
                g_string_append(help_out, "Error: Invalid command.\r\n");
            }
        }
    }

    // Send help output through pager
    cli_pager_output(session, help_out->str);
    g_string_free(help_out, TRUE);

    // Truncate line_buffer at cursor position
    *line_pos = cursor_pos;
    line_buffer[*line_pos] = '\0';

    // If pager is active, it will handle prompt when done
    if (!session->pager_active)
    {
        // Clear current line and redraw with proper cursor position
        cfg_send_message(session, "\r");      // Move to start of line
        cfg_send_message(session, "\x1B[2K"); // Clear entire line

        send_prompt(session);
        if (*line_pos > 0)
        {
            cfg_send_data(session, line_buffer, *line_pos);
        }
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

    if (*str == ERRCODE_SUCCESS)
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
int process_command(const char *cmd_line, cli_session_t *session)
{
    char buffer[MAX_CMD_LEN];
    strncpy(buffer, cmd_line, MAX_CMD_LEN - 1);
    buffer[MAX_CMD_LEN - 1] = '\0';

    char *trimmed = trim(buffer);

    // Empty command
    if (strlen(trimmed) == ERRCODE_SUCCESS)
    {
        return 0; // Empty command, don't record
    }

    // 以 # 开头的行视为注释，不做任何处理
    if (trimmed[0] == '#')
    {
        return 0;
    }

    // Get current view's command tree
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        cfg_send_message(session, "\r\nError: No command tree for current view\r\n");
        return 0; // Error
    }

    // Use full match to get all element IDs and values
    cli_match_result_t *match_result = cli_tree_match_command_full(session->current_view->cmd_tree, trimmed);
    cli_tree_node_t *node = match_result ? match_result->final_node : NULL;

    if (node)
    {
        // Check if command is complete (node must be marked as an end node)
        if (!node->is_end_node)
        {
            // Incomplete command - node is not a valid command end point
            cfg_send_message(session, "Error: Incomplete command.\r\n");

            // Free match result and return
            if (match_result)
            {
                cli_match_result_free(match_result);
            }
            return 0; // Error
        }

        // Dispatch to module if module_id is set
        if (match_result && match_result->module_id != 0)
        {
            if (match_result->module_id == DEV_MODULE_ID_CFG)
            {
                cfg_cli_handle(match_result, session);
            }
            else
            {
                cli_dispatch_to_module(match_result, session);
            }
        }

        // Free match result
        if (match_result)
        {
            cli_match_result_free(match_result);
        }
        return 1; // Success
    }
    else
    {
        cfg_send_message(session, "Error: Invalid command.\r\n");

        // Free match result
        if (match_result)
        {
            cli_match_result_free(match_result);
        }
        return 0; // Error
    }
}

// Cleanup CLI trees
void cli_cleanup(void)
{
    if (g_cli_engine_ctx == NULL)
    {
        return;
    }
    if (g_cli_engine_ctx && g_cli_engine_ctx->view_tree.root)
    {
        cli_view_free(g_cli_engine_ctx->view_tree.root);
        g_cli_engine_ctx->view_tree.root = NULL;
    }
    if (g_cli_engine_ctx && g_cli_engine_ctx->view_tree.global_view)
    {
        cli_view_free(g_cli_engine_ctx->view_tree.global_view);
        g_cli_engine_ctx->view_tree.global_view = NULL;
    }
}

// Create a new client session
cli_session_t *cli_session_create(int client_fd)
{
    cli_session_t *session = g_malloc0(sizeof(cli_session_t));
    if (!session)
    {
        return NULL;
    }

    // Set non-blocking mode
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    session->client_fd = client_fd;
    session->current_view = g_cli_engine_ctx ? g_cli_engine_ctx->view_tree.root : NULL;
    session->line_pos = 0;
    session->cursor_pos = 0;
    session->state = CLI_STATE_NORMAL;

    if (session->current_view && session->current_view->prompt_template)
    {
        update_prompt_from_template(session, session->current_view->prompt_template);
    }
    else
    {
        update_prompt_from_template(session, "NetNexus>");
    }
    cli_session_history_init(&session->history);

    // Initialize pager
    session->pager_buffer = NULL;
    session->pager_offset = 0;
    session->pager_total_len = 0;
    session->pager_lines_per_page = CLI_PAGER_DEFAULT_LINES;
    session->pager_active = 0;

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
    cfg_send_data(session, telnet_opts, sizeof(telnet_opts));

    // Send welcome message
    cfg_send_message(session, "\r\n");
    cfg_send_message(session, "Welcome to NetNexus CLI\r\n");
    cfg_send_message(session, "Type '?' for available commands\r\n");
    cfg_send_message(session, "\r\n");

    // Send initial prompt
    send_prompt(session);

    return session;
}

// Process available input for a session
// Returns: 0 on success, -1 on disconnect/error
int cli_process_input(cli_session_t *session)
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

        // Handle pager input if active (intercept before normal processing)
        if (session->pager_active)
        {
            pager_handle_key(session, c);
            continue;
        }

        // State machine for ANSI escape sequences
        if (session->state == CLI_STATE_NORMAL)
        {
            // Check for ESC key (start of escape sequence)
            if (c == 27)
            {
                session->state = CLI_STATE_ESC;
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
                cfg_send_message(session, "\r\n");

                if (session->line_pos > 0)
                {
                    session->line_buffer[session->line_pos] = '\0';

                    // Process command and add to history only if successful
                    int cmd_success = process_command(session->line_buffer, session);
                    // Add to local session history
                    cli_session_history_add(&session->history, session->line_buffer, session->client_ip);
                    if (cmd_success)
                    {
                        // Add to global history (thread-safe)
                        pthread_mutex_lock(&g_cli_engine_ctx->history_mutex);
                        cli_global_history_add(&g_cli_engine_ctx->global_history, session->line_buffer,
                                               session->current_view->view_id);
                        pthread_mutex_unlock(&g_cli_engine_ctx->history_mutex);
                    }

                    // Reset line buffer
                    session->line_pos = 0;
                    session->cursor_pos = 0;
                    session->history.browse_idx = -1; // Reset browse state
                }

                // Don't send prompt if pager is active (pager will send it when done)
                if (!session->pager_active)
                {
                    send_prompt(session);
                }
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
                        cfg_send_message(session, "\b");
                        redraw_from_cursor(session, session->line_buffer, session->cursor_pos, session->line_pos);
                    }
                    else
                    {
                        // Delete at end (original logic)
                        session->line_pos--;
                        session->cursor_pos--;
                        cfg_send_message(session, "\b \b");
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
                    cfg_send_data(session, &c, 1);
                }
            }
        }
        else if (session->state == CLI_STATE_ESC)
        {
            // After ESC, expect '[' for CSI sequence
            if (c == '[')
            {
                session->state = CLI_STATE_CSI;
            }
            else
            {
                // Not a CSI sequence, ignore and reset
                session->state = CLI_STATE_NORMAL;
            }
        }
        else if (session->state == CLI_STATE_CSI)
        {
            // Handle arrow keys
            if (c == 'A')
            {
                // Up arrow
                handle_arrow_up(session, session->line_buffer, &session->line_pos, &session->cursor_pos);
                session->state = CLI_STATE_NORMAL;
            }
            else if (c == 'B')
            {
                // Down arrow
                handle_arrow_down(session, session->line_buffer, &session->line_pos, &session->cursor_pos);
                session->state = CLI_STATE_NORMAL;
            }
            else if (c == 'C')
            {
                // Right arrow
                handle_arrow_right(session, session->line_pos, &session->cursor_pos);
                session->state = CLI_STATE_NORMAL;
            }
            else if (c == 'D')
            {
                // Left arrow
                handle_arrow_left(session, &session->cursor_pos);
                session->state = CLI_STATE_NORMAL;
            }
            else
            {
                // Unknown CSI sequence, ignore
                session->state = CLI_STATE_NORMAL;
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
void cli_session_destroy(cli_session_t *session)
{
    if (!session)
    {
        return;
    }

    cli_session_history_cleanup(&session->history);

    // 释放所有上下文栈数据
    for (uint32_t i = 0; i < CLI_PROMPT_STACK_DEPTH; i++)
    {
        if (session->view_context_stack[i])
        {
            g_free(session->view_context_stack[i]);
            session->view_context_stack[i] = NULL;
            session->view_context_len[i] = 0;
        }
    }

    // Clean up pager
    if (session->pager_buffer)
    {
        g_free(session->pager_buffer);
        session->pager_buffer = NULL;
    }

    if (session->client_fd >= 0)
    {
        close(session->client_fd);
    }
    g_free(session);
}
