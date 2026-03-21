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
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cli_cfg.h"
#include "cli_dispatch.h"
#include "cli_main.h"
#include "cli_param_type.h"
#include "cli_tree.h"
#include "cli_xml_parser.h"
#include "dev.h"
#include "errcode.h"

static void cli_write_best_effort(int fd, const void *data, size_t len)
{
    if (fd < 0 || !data || len == 0)
    {
        return;
    }

    const char *buf = (const char *)data;
    while (len > 0)
    {
        ssize_t n = write(fd, buf, len);
        if (n > 0)
        {
            buf += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        break;
    }
}

// Send a message to the client (must be null-terminated)
void cli_send_message(cli_session_t *session, const char *message)
{
    if (message && session)
    {
        cli_write_best_effort(session->client_fd, message, strlen(message));
    }
}

// Send raw data to the client with explicit length
void cli_send_data(cli_session_t *session, const void *data, size_t len)
{
    if (data && len > 0 && session)
    {
        cli_write_best_effort(session->client_fd, data, len);
    }
}

// Update session prompt from module-filled template (module has already resolved placeholders like %u)
void update_prompt_from_template(cli_session_t *session, const char *module_prompt)
{
    if (!session || !module_prompt)
    {
        return;
    }

    g_strlcpy(session->prompt, module_prompt, sizeof(session->prompt));
}

// Push current prompt onto the stack (call before entering a sub-view)
void cli_prompt_push(cli_session_t *session)
{
    if (!session || session->prompt_stack_depth >= CLI_PROMPT_STACK_DEPTH)
    {
        return;
    }

    g_strlcpy(session->prompt_stack[session->prompt_stack_depth], session->prompt, CLI_CLI_MAX_PROMPT_LEN);

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

    g_strlcpy(session->prompt, session->prompt_stack[session->prompt_stack_depth], sizeof(session->prompt));
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
    cli_send_message(session, session->prompt);
    cli_send_message(session, " ");
}

// ============================================================================
// Pager (--More--) implementation
// ============================================================================

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
    cli_send_message(session, CLI_PAGER_PROMPT);
}

// Clear the --More-- prompt from the line
static void pager_clear_more_prompt(cli_session_t *session)
{
    // \r moves to start, spaces overwrite, \r moves back to start
    cli_send_message(session, "\r        \r");
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
        cli_send_data(session, session->pager_buffer + start, pos - start);
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

    /* session pager disabled */
    if (session->pager_lines_per_page == 0)
    {
        cli_send_message(session, message);
        return;
    }

    uint32_t lines = pager_count_lines(message);
    uint32_t page_size = session->pager_lines_per_page;

    // If output fits on one screen, send directly
    if (lines <= page_size)
    {
        cli_send_message(session, message);
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
        cli_send_data(session, buffer + from_pos, total_len - from_pos);
    }

    // Send a space to clear any old character
    cli_send_message(session, " ");

    // Move cursor back to correct position
    uint32_t move_back = total_len - from_pos + 1;
    for (uint32_t i = 0; i < move_back; i++)
    {
        cli_send_message(session, "\b");
    }
}

// Clear current line and redraw with new content
static void clear_and_redraw_line(cli_session_t *session, const char *buffer, uint32_t len, uint32_t cursor_pos)
{
    // Move to start of line and clear it
    cli_send_message(session, "\r");
    cli_send_message(session, ANSI_CLEAR_LINE);

    // Redraw prompt
    send_prompt(session);

    // Redraw buffer content
    if (len > 0)
    {
        cli_send_data(session, buffer, len);
    }

    // Move cursor to correct position
    if (cursor_pos < len)
    {
        uint32_t move_back = len - cursor_pos;
        for (uint32_t i = 0; i < move_back; i++)
        {
            cli_send_message(session, "\b");
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
        g_strlcpy(history->temp_buffer, line_buffer, MAX_CMD_LEN);

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
        g_strlcpy(line_buffer, hist_cmd, MAX_CMD_LEN);
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
            g_strlcpy(line_buffer, hist_cmd, MAX_CMD_LEN);
            *line_pos = strlen(line_buffer);
            *cursor_pos = *line_pos;

            clear_and_redraw_line(session, line_buffer, *line_pos, *cursor_pos);
        }
    }
    else
    {
        // Back to current input (restore temp_buffer)
        history->browse_idx = -1;

        g_strlcpy(line_buffer, history->temp_buffer, MAX_CMD_LEN);
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
        cli_send_message(session, "\b");
    }
}

// Handle right arrow key - move cursor right
static void handle_arrow_right(cli_session_t *session, uint32_t line_pos, uint32_t *cursor_pos)
{
    if (*cursor_pos < line_pos)
    {
        cli_send_message(session, ANSI_CURSOR_RIGHT);
        (*cursor_pos)++;
    }
}

// ============================================================================
// 动态补全辅助函数
// ============================================================================

/**
 * @brief 向目标模块发起同步 RPC，查询动态候选值列表
 *
 * 目标模块和 query_id 优先从 param_type->range.dynamic 中读取；
 * 若为 0（同模块场景），则回退到 node 自身的 module_id / cfg_id。
 *
 * @param node 动态参数树节点（含 module_id、cfg_id 及 param_type）
 * @return NULL 结尾的字符串数组（g_strfreev 释放），失败返回 NULL
 */
static char **cfg_query_dynamic_candidates(const cli_tree_node_t *node)
{
    const cli_param_type_t *pt = node->param_type;

    /* 优先使用 param_type 中显式指定的目标模块和 query_id */
    uint32_t module_id = pt->range.dynamic.candidates_module_id;
    if (module_id == 0)
    {
        module_id = node->module_id;
    }

    uint32_t query_id = pt->range.dynamic.candidates_query_id;
    if (query_id == 0)
    {
        query_id = node->cfg_id;
    }

    uint32_t *payload = g_new(uint32_t, 1);
    *payload = g_htonl(query_id);

    dev_ipc_message_t *req = dev_ipc_message_create(CLI_MSG_TYPE_QUERY_CANDIDATES, DEV_MODULE_ID_CLI, module_id, 0,
                                                    payload, sizeof(uint32_t), g_free);
    if (!req)
    {
        g_free(payload);
        return NULL;
    }

    dev_ipc_message_t *resp = dev_ipc_query(g_cli_local->dev_ipc_ctx, module_id, req, 2000);
    dev_ipc_message_free(req);

    if (!resp || !resp->payload || resp->payload_len < 2)
    {
        if (resp)
        {
            dev_ipc_message_free(resp);
        }
        return NULL;
    }

    /* 解析 "val1\0val2\0\0" 格式 */
    GPtrArray *arr = g_ptr_array_new();
    const char *p = (const char *)resp->payload;
    const char *end = p + resp->payload_len;

    while (p < end && *p != '\0')
    {
        g_ptr_array_add(arr, g_strdup(p));
        p += strlen(p) + 1;
    }

    g_ptr_array_add(arr, NULL); /* NULL 结尾 */
    char **result = (char **)g_ptr_array_free(arr, FALSE);

    dev_ipc_message_free(resp);
    return result;
}

/**
 * @brief 将动态候选值填入行缓冲区（始终替换最后一个 token，兼容尾部空格）
 * @param candidate   候选字符串
 * @param line_buffer 行缓冲区（in/out）
 * @param line_pos    行长度（in/out）
 */
static void tab_apply_dynamic_match(const char *candidate, char *line_buffer, uint32_t *line_pos)
{
    /* 向前跳过尾部空格，找到最后一个有效字符的位置 */
    uint32_t search_end = *line_pos;
    while (search_end > 0 && line_buffer[search_end - 1] == ' ')
    {
        search_end--;
    }

    /* 从有效字符起向前找最近的空格，确定最后一个 token 的起始位置 */
    uint32_t token_start = 0;
    for (uint32_t i = search_end; i > 0; i--)
    {
        if (line_buffer[i - 1] == ' ')
        {
            token_start = i;
            break;
        }
    }
    *line_pos = token_start;

    for (uint32_t i = 0; candidate[i] && *line_pos < MAX_CMD_LEN - 1; i++)
    {
        line_buffer[*line_pos] = candidate[i];
        (*line_pos)++;
    }
    if (*line_pos < MAX_CMD_LEN - 1)
    {
        line_buffer[*line_pos] = ' ';
        (*line_pos)++;
    }
    line_buffer[*line_pos] = '\0';
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
        if (!child->description)
        {
            continue;
        }

        /* 动态参数：先显示内层类型范围，再逐条展示 RPC 候选值 */
        if (child->type == CLI_NODE_ARGUMENT && child->param_type && child->param_type->type == PARAM_TYPE_DYNAMIC)
        {
            /* 首行：展示内层类型约束，提示用户输入合法范围 */
            const cli_param_type_t *inner = child->param_type->range.dynamic.inner;
            char type_display[64];
            if (inner && inner->type_str)
            {
                snprintf(type_display, sizeof(type_display), "<%s>", inner->type_str);
            }
            else
            {
                snprintf(type_display, sizeof(type_display), "<%s>", child->param_type->type_str);
            }
            snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", type_display, child->description);
            g_string_append(out, buffer);

            /* 后续行：RPC 查询并展示当前已有候选值 */
            char **candidates = cfg_query_dynamic_candidates(child);
            if (candidates)
            {
                for (uint32_t j = 0; candidates[j]; j++)
                {
                    snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", candidates[j], child->description);
                    g_string_append(out, buffer);
                }
                g_strfreev(candidates);
            }
            continue;
        }

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
            g_strlcpy(name_display, child->name, sizeof(name_display));
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
            g_strlcpy(desc_with_marker, child->description, sizeof(desc_with_marker));
        }

        snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", name_display, desc_with_marker);
        g_string_append(out, buffer);
    }
}

// Apply a tab completion match to the line buffer
static void tab_apply_match(cli_tree_node_t *match, char *line_buffer, uint32_t *line_pos, uint32_t has_trailing_space)
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

    // Get all matches for the last token（view tree + global tree 双树查找）
    cli_tree_node_t *matches[50];
    cli_tree_node_t *global_tree = g_cli_local->view_tree.global_cmd_tree;
    uint32_t num_matches =
        cli_tree_match_command_get_matches_dual(session->current_view->cmd_tree, global_tree, match_input, matches, 50);

    // Check if we have a trailing space in the original input
    uint32_t orig_len = session->tab_cycling ? session->tab_original_pos : *line_pos;
    uint32_t has_trailing_space = (orig_len > 0 && match_input[orig_len - 1] == ' ');

    /* 检查是否存在 DYNAMIC ARGUMENT 匹配 */
    cli_tree_node_t *dynamic_match = NULL;
    for (uint32_t i = 0; i < num_matches; i++)
    {
        if (matches[i]->type == CLI_NODE_ARGUMENT && matches[i]->param_type &&
            matches[i]->param_type->type == PARAM_TYPE_DYNAMIC)
        {
            dynamic_match = matches[i];
            break;
        }
    }

    if (dynamic_match)
    {
        /* 获取用于前缀过滤的当前 token */
        const char *prefix = "";
        char prefix_buf[MAX_CMD_LEN];
        if (!has_trailing_space)
        {
            char *last_space = strrchr(match_input, ' ');
            prefix = last_space ? last_space + 1 : match_input;
        }
        else
        {
            /* 尾部有空格（动态补全后自动追加的空格）：
             * 取紧邻空格前的最后一个 token 作为前缀，
             * 避免 prefix 为空串导致所有候选都被匹配 */
            int search_end = (int)orig_len - 1;
            while (search_end >= 0 && match_input[search_end] == ' ')
            {
                search_end--;
            }
            int token_start = 0;
            for (int i = search_end; i > 0; i--)
            {
                if (match_input[i - 1] == ' ')
                {
                    token_start = i;
                    break;
                }
            }
            if (search_end >= token_start)
            {
                int token_len = search_end - token_start + 1;
                memcpy(prefix_buf, match_input + token_start, token_len);
                prefix_buf[token_len] = '\0';
                prefix = prefix_buf;
            }
        }
        g_strlcpy(prefix_buf, prefix, sizeof(prefix_buf));
        uint32_t prefix_len = (uint32_t)strlen(prefix_buf);

        /* RPC 查询候选值 */
        char **candidates = cfg_query_dynamic_candidates(dynamic_match);
        if (!candidates)
        {
            session->tab_cycling = 0;
            cli_send_message(session, "\r\n");
            send_prompt(session);
            cli_send_message(session, line_buffer);
        }
        else
        {
            /* 收集匹配前缀的候选值 */
            GPtrArray *matched = g_ptr_array_new();
            for (uint32_t i = 0; candidates[i]; i++)
            {
                if (strncmp(candidates[i], prefix_buf, prefix_len) == 0)
                {
                    g_ptr_array_add(matched, candidates[i]);
                }
            }

            uint32_t num_dyn = matched->len;
            if (num_dyn == 0)
            {
                session->tab_cycling = 0;
                cli_send_message(session, "\r\n");
                send_prompt(session);
                cli_send_message(session, line_buffer);
            }
            else if (num_dyn == 1)
            {
                session->tab_cycling = 0;
                /* 保证缓冲区从原始输入状态开始替换 */
                memcpy(line_buffer, match_input, orig_len);
                *line_pos = orig_len;
                line_buffer[*line_pos] = '\0';
                cli_send_message(session, "\r\n");
                send_prompt(session);
                tab_apply_dynamic_match((const char *)matched->pdata[0], line_buffer, line_pos);
                cli_send_message(session, line_buffer);
            }
            else
            {
                /* 多个候选：循环补全 */
                if (!session->tab_cycling)
                {
                    session->tab_cycling = 1;
                    session->tab_match_index = 0;
                    memcpy(session->tab_original, line_buffer, *line_pos);
                    session->tab_original_pos = *line_pos;
                }
                else
                {
                    session->tab_match_index = (session->tab_match_index + 1) % num_dyn;
                }

                memcpy(line_buffer, session->tab_original, session->tab_original_pos);
                *line_pos = session->tab_original_pos;
                line_buffer[*line_pos] = '\0';

                cli_send_message(session, "\r\n");
                send_prompt(session);
                tab_apply_dynamic_match((const char *)matched->pdata[session->tab_match_index], line_buffer, line_pos);
                cli_send_message(session, line_buffer);
            }

            g_ptr_array_free(matched, FALSE);
            g_strfreev(candidates);
        }
    }
    else if (num_matches == 1)
    {
        // Single match - auto-complete directly
        session->tab_cycling = 0;
        cli_tree_node_t *match = matches[0];

        cli_send_message(session, "\r\n");
        send_prompt(session);

        tab_apply_match(match, line_buffer, line_pos, has_trailing_space);
        cli_send_message(session, line_buffer);
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

        cli_send_message(session, "\r\n");
        send_prompt(session);

        tab_apply_match(match, line_buffer, line_pos, has_trailing_space);
        cli_send_message(session, line_buffer);
    }
    else
    {
        session->tab_cycling = 0;
        cli_send_message(session, "\r\n");
        send_prompt(session);
        cli_send_message(session, line_buffer);
    }
}

// Handle ? key for help
static void handle_help_request(cli_session_t *session, char *line_buffer, uint32_t *line_pos, uint32_t cursor_pos)
{
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        return;
    }

    cli_send_message(session, "\r\n");

    // Create temp buffer with only [0, cursor_pos) for matching
    char match_buffer[MAX_CMD_LEN];
    memcpy(match_buffer, line_buffer, cursor_pos);
    match_buffer[cursor_pos] = '\0';

    // Check if we have a trailing space in the cursor range
    uint32_t has_trailing_space = (cursor_pos > 0 && match_buffer[cursor_pos - 1] == ' ');
    char buffer[256];

    // Collect all help output into a GString for paging
    GString *help_out = g_string_new("");

    cli_tree_node_t *help_global_tree = g_cli_local->view_tree.global_cmd_tree;
    if (has_trailing_space)
    {
        // Case: "xx ?" - 在 view tree 和 global tree 中各查一次，分别展示子命令
        cli_tree_node_t *ctx_view = cli_tree_match_command(session->current_view->cmd_tree, match_buffer);
        cli_tree_node_t *ctx_global = help_global_tree ? cli_tree_match_command(help_global_tree, match_buffer) : NULL;

        if (ctx_view || ctx_global)
        {
            if (ctx_view)
            {
                cli_tree_print_help(ctx_view, help_out);
            }
            if (ctx_global)
            {
                cli_tree_print_help(ctx_global, help_out);
            }
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
        uint32_t num_matches = cli_tree_match_command_get_matches_dual(session->current_view->cmd_tree,
                                                                       help_global_tree, match_buffer, matches, 50);

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

                /* 动态参数：先显示内层类型范围，再按前缀过滤 RPC 候选值 */
                if (arg->param_type && arg->param_type->type == PARAM_TYPE_DYNAMIC)
                {
                    /* 提取当前 token 作为前缀 */
                    const char *prefix = "";
                    char *last_space = strrchr(match_buffer, ' ');
                    if (last_space)
                    {
                        prefix = last_space + 1;
                    }
                    else if (cursor_pos > 0)
                    {
                        prefix = match_buffer;
                    }
                    uint32_t prefix_len = (uint32_t)strlen(prefix);

                    /* 首行：内层类型约束（前缀匹配时才显示） */
                    const cli_param_type_t *inner = arg->param_type->range.dynamic.inner;
                    char type_display[64];
                    if (inner && inner->type_str)
                    {
                        snprintf(type_display, sizeof(type_display), "<%s>", inner->type_str);
                    }
                    else
                    {
                        snprintf(type_display, sizeof(type_display), "<%s>", arg->param_type->type_str);
                    }
                    if (strncmp(type_display + 1, prefix, prefix_len) == 0 || prefix_len == 0)
                    {
                        snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", type_display,
                                 arg->description ? arg->description : "");
                        g_string_append(help_out, buffer);
                    }

                    /* 后续行：RPC 候选值按前缀过滤 */
                    char **candidates = cfg_query_dynamic_candidates(arg);
                    if (candidates)
                    {
                        for (uint32_t ci = 0; candidates[ci]; ci++)
                        {
                            if (strncmp(candidates[ci], prefix, prefix_len) == 0)
                            {
                                snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", candidates[ci],
                                         arg->description ? arg->description : "");
                                g_string_append(help_out, buffer);
                            }
                        }
                        g_strfreev(candidates);
                    }
                }
                else
                {
                    char name_display[128];
                    if (arg->param_type && arg->param_type->type_str)
                    {
                        snprintf(name_display, sizeof(name_display), "<%s>", arg->param_type->type_str);
                    }
                    else if (arg->name)
                    {
                        g_strlcpy(name_display, arg->name, sizeof(name_display));
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
                /* 空输入 "?"：展示 view tree 和 global tree 的所有根级子命令 */
                if (session->current_view->cmd_tree)
                {
                    cli_tree_print_help(session->current_view->cmd_tree, help_out);
                }
                if (help_global_tree)
                {
                    cli_tree_print_help(help_global_tree, help_out);
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
        cli_send_message(session, "\r");      // Move to start of line
        cli_send_message(session, "\x1B[2K"); // Clear entire line

        send_prompt(session);
        if (*line_pos > 0)
        {
            cli_send_data(session, line_buffer, *line_pos);
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
    g_strlcpy(buffer, cmd_line, MAX_CMD_LEN);

    char *trimmed = trim(buffer);

    // Empty command
    if (strlen(trimmed) == ERRCODE_SUCCESS)
    {
        return 0; // Empty command, don't record
    }

    // 以 ! 开头的行视为注释，不做任何处理
    if (trimmed[0] == '!')
    {
        return 0;
    }

    // Get current view's command tree
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        cli_send_message(session, "\r\nError: No command tree for current view\r\n");
        return 0; // Error
    }

    // Use full match to get all element IDs and values（view tree 优先，失败后回退 global tree）
    cli_match_result_t *match_result = cli_tree_match_command_full_dual(
        session->current_view->cmd_tree, g_cli_local->view_tree.global_cmd_tree, trimmed);
    cli_tree_node_t *node = match_result ? match_result->final_node : NULL;

    if (node)
    {
        // Check if command is complete (node must be marked as an end node)
        if (!node->is_end_node)
        {
            // Incomplete command - node is not a valid command end point
            cli_send_message(session, "Error: Incomplete command.\r\n");

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
            cli_dispatch_to_module(match_result, session);
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
        cli_send_message(session, "Error: Invalid command.\r\n");

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
    if (g_cli_local == NULL)
    {
        return;
    }
    if (g_cli_local->view_tree.root)
    {
        cli_view_free(g_cli_local->view_tree.root);
        g_cli_local->view_tree.root = NULL;
    }
    if (g_cli_local->view_tree.global_view)
    {
        cli_view_free(g_cli_local->view_tree.global_view);
        g_cli_local->view_tree.global_view = NULL;
        g_cli_local->view_tree.global_cmd_tree = NULL; /* 非持有指针，随 global_view 释放 */
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
    session->current_view = g_cli_local->view_tree.root;
    session->line_pos = 0;
    session->cursor_pos = 0;
    session->state = CLI_STATE_NORMAL;

    update_prompt_from_template(session, session->current_view->prompt_template);
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
    cli_send_data(session, telnet_opts, sizeof(telnet_opts));

    // Send welcome message
    cli_send_message(session, "\r\n");
    cli_send_message(session, "Welcome to NetNexus CLI\r\n");
    cli_send_message(session, "Type '?' for available commands\r\n");
    cli_send_message(session, "\r\n");

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
        // 过滤 Telnet 协议协商字节（IAC 序列）
        // IAC = 0xFF，序列格式：
        //   IAC WILL/WONT/DO/DONT <option>  → 3 字节
        //   IAC NOP/DM/BRK/...              → 2 字节（单字节命令）
        //   IAC SB <option> <data> IAC SE   → 变长子协商
        if ((unsigned char)c == 255)
        { // IAC
            char cmd;
            if (read(session->client_fd, &cmd, 1) < 1)
            {
                continue;
            }
            unsigned char ucmd = (unsigned char)cmd;
            if (ucmd == 250)
            { // SB：子协商，读取直到 IAC SE（0xFF 0xF0）
                char prev = 0, cur;
                while (read(session->client_fd, &cur, 1) == 1)
                {
                    if ((unsigned char)prev == 255 && (unsigned char)cur == 240)
                    {
                        break; // IAC SE，子协商结束
                    }
                    prev = cur;
                }
            }
            else if (ucmd >= 251 && ucmd <= 254)
            { // WILL/WONT/DO/DONT：后跟 1 字节 option
                char opt;
                ssize_t opt_read = read(session->client_fd, &opt, 1);
                if (opt_read < 1)
                {
                    continue;
                }
            }
            // 其他单字节命令（NOP、DM、BRK 等）直接丢弃，无需额外读取
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
                cli_send_message(session, "\r\n");

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
                        pthread_mutex_lock(&g_cli_local->history_mutex);
                        cli_global_history_add(&g_cli_local->global_history, session->line_buffer, session->client_ip);
                        pthread_mutex_unlock(&g_cli_local->history_mutex);
                    }

                    // Reset line buffer
                    session->line_pos = 0;
                    session->cursor_pos = 0;
                    session->history.browse_idx = -1; // Reset browse state
                }

                // pager 或 bash 模式激活时不发送提示符（由对应子系统负责）
                if (!session->pager_active && !session->bash_mode)
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
                        cli_send_message(session, "\b");
                        redraw_from_cursor(session, session->line_buffer, session->cursor_pos, session->line_pos);
                    }
                    else
                    {
                        // Delete at end (original logic)
                        session->line_pos--;
                        session->cursor_pos--;
                        cli_send_message(session, "\b \b");
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
                    // Redraw from new cursor_pos (the char is already at cursor_pos-1)
                    cli_send_data(session, &c, 1);
                    redraw_from_cursor(session, session->line_buffer, session->cursor_pos, session->line_pos);
                }
                else
                {
                    // Append at end (original logic)
                    session->line_buffer[session->line_pos++] = c;
                    session->cursor_pos++;
                    cli_send_data(session, &c, 1);
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
