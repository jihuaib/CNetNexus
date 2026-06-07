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

// ACCESS 架构：命令输出不再直接写 socket，而是累积到 session->out 缓冲，
// 由 LINE_INPUT 处理完成后经 IPC 回传给 ACCESS 显示。
// （cli_write_best_effort 及下方终端函数为旧 telnet 直连遗留，ACCESS 架构下不再走到，
//  待 P5 清理；保留以最小化本阶段改动面。）
void cli_send_message(cli_session_t *session, const char *message)
{
    if (message && session && session->out)
    {
        g_string_append(session->out, message);
    }
}

// Send raw data to the client with explicit length
void cli_send_data(cli_session_t *session, const void *data, size_t len)
{
    if (data && len > 0 && session && session->out)
    {
        g_string_append_len(session->out, (const char *)data, (gssize)len);
    }
}

// 渲染当前提示符：把模板里的 sysname 占位（默认 "NetNexus"）替换为实际系统名，
// 结果写入 out（不含尾随空格，尾随空格由 ACCESS line 层补）。供 IPC 响应携带。
void cli_render_prompt(cli_session_t *session, char *out, size_t out_size)
{
    if (!session || !out || out_size == 0)
    {
        return;
    }
    const char *sysname = (g_cli_local && g_cli_local->sysname[0] != '\0') ? g_cli_local->sysname : CLI_SYSNAME_DEFAULT;

    if (strcmp(sysname, CLI_SYSNAME_DEFAULT) == 0 || !strstr(session->prompt, CLI_SYSNAME_DEFAULT))
    {
        g_strlcpy(out, session->prompt, out_size);
        return;
    }

    size_t out_pos = 0;
    const char *src = session->prompt;
    size_t pat_len = strlen(CLI_SYSNAME_DEFAULT);
    size_t name_len = strlen(sysname);
    while (*src && out_pos + 1 < out_size)
    {
        if (strncmp(src, CLI_SYSNAME_DEFAULT, pat_len) == 0)
        {
            size_t copy = (out_pos + name_len < out_size - 1) ? name_len : (out_size - 1 - out_pos);
            memcpy(out + out_pos, sysname, copy);
            out_pos += copy;
            src += pat_len;
        }
        else
        {
            out[out_pos++] = *src++;
        }
    }
    out[out_pos] = '\0';
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
//
// 渲染时把模板里的占位符 "NetNexus" 替换为当前 g_cli_local->sysname。
// 这样 sysname 命令一变，所有 session 下次出 prompt 立即生效，
// 无需重建每个 view 的模板。
void send_prompt(cli_session_t *session)
{
    const char *sysname = (g_cli_local && g_cli_local->sysname[0] != '\0') ? g_cli_local->sysname : CLI_SYSNAME_DEFAULT;

    if (strcmp(sysname, CLI_SYSNAME_DEFAULT) == 0 || !strstr(session->prompt, CLI_SYSNAME_DEFAULT))
    {
        cli_send_message(session, session->prompt);
    }
    else
    {
        char rendered[CLI_CLI_MAX_PROMPT_LEN * 2];
        const char *src = session->prompt;
        size_t out_pos = 0;
        size_t pat_len = strlen(CLI_SYSNAME_DEFAULT);
        size_t name_len = strlen(sysname);
        while (*src && out_pos + 1 < sizeof(rendered))
        {
            if (strncmp(src, CLI_SYSNAME_DEFAULT, pat_len) == 0)
            {
                size_t copy = (out_pos + name_len < sizeof(rendered) - 1) ? name_len : (sizeof(rendered) - 1 - out_pos);
                memcpy(rendered + out_pos, sysname, copy);
                out_pos += copy;
                src += pat_len;
            }
            else
            {
                rendered[out_pos++] = *src++;
            }
        }
        rendered[out_pos] = '\0';
        cli_send_message(session, rendered);
    }
    cli_send_message(session, " ");
}

// ACCESS 架构：分页（--More--）由 ACCESS line 层按 per-line length 完成。
// CLI 这里只负责把完整输出文本累积到 out 缓冲，整段回传给 ACCESS。
void cli_pager_output(cli_session_t *session, const char *message)
{
    if (!session || !message || message[0] == '\0')
    {
        return;
    }
    cli_send_message(session, message);
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

// ============================================================================
// ACCESS line 层用的补全/帮助计算（只做命令树匹配，结果回传 ACCESS 渲染）
// ============================================================================

/**
 * @brief 计算 Tab 补全候选（最后一个 token 的候选 token 列表）
 * @param session 逻辑会话（提供 current_view）
 * @param partial 光标前的输入文本
 * @param out     输出缓冲，写入 "tok1\0tok2\0..." 形式（每个候选以 \0 结尾）
 */
void cli_build_tab_candidates(cli_session_t *session, const char *partial, GString *out)
{
    if (!session || !session->current_view || !session->current_view->cmd_tree || !partial || !out)
    {
        return;
    }

    char match_input[MAX_CMD_LEN];
    g_strlcpy(match_input, partial, sizeof(match_input));

    cli_tree_node_t *matches[50];
    cli_tree_node_t *global_tree = g_cli_local->view_tree.global_cmd_tree;
    uint32_t num_matches =
        cli_tree_match_command_get_matches_dual(session->current_view->cmd_tree, global_tree, match_input, matches, 50);

    /* 计算当前 token 前缀；行尾空格表示补全下一 token，前缀应为空。 */
    uint32_t e = (uint32_t)strlen(match_input);
    char prefix_buf[MAX_CMD_LEN];
    uint32_t prefix_len = 0;
    if (e == 0 || isspace((unsigned char)match_input[e - 1]))
    {
        prefix_buf[0] = '\0';
    }
    else
    {
        uint32_t st = e;
        while (st > 0 && !isspace((unsigned char)match_input[st - 1]))
        {
            st--;
        }
        prefix_len = e - st;
        memcpy(prefix_buf, match_input + st, prefix_len);
        prefix_buf[prefix_len] = '\0';
    }

    /* 动态参数：RPC 取候选值，按前缀过滤后回传 */
    for (uint32_t i = 0; i < num_matches; i++)
    {
        if (matches[i]->type == CLI_NODE_ARGUMENT && matches[i]->param_type &&
            matches[i]->param_type->type == PARAM_TYPE_DYNAMIC)
        {
            char **candidates = cfg_query_dynamic_candidates(matches[i]);
            if (candidates)
            {
                for (uint32_t j = 0; candidates[j]; j++)
                {
                    if (strncmp(candidates[j], prefix_buf, prefix_len) == 0)
                    {
                        g_string_append(out, candidates[j]);
                        g_string_append_c(out, '\0');
                    }
                }
                g_strfreev(candidates);
            }
            return;
        }
    }

    /* 关键字候选：匹配到的 COMMAND 节点名 */
    for (uint32_t i = 0; i < num_matches; i++)
    {
        if (matches[i]->type == CLI_NODE_COMMAND && matches[i]->name)
        {
            g_string_append(out, matches[i]->name);
            g_string_append_c(out, '\0');
        }
    }
}

/**
 * @brief 构建 '?' 帮助文本（不做终端渲染，文本回传 ACCESS 分页显示）
 * @param session 逻辑会话
 * @param partial 光标前的输入文本
 * @param out     输出帮助文本
 */
void cli_build_help_text(cli_session_t *session, const char *partial, GString *out)
{
    if (!session || !session->current_view || !session->current_view->cmd_tree || !partial || !out)
    {
        return;
    }

    char match_buffer[MAX_CMD_LEN];
    g_strlcpy(match_buffer, partial, sizeof(match_buffer));
    uint32_t cursor_pos = (uint32_t)strlen(match_buffer);
    uint32_t has_trailing_space = (cursor_pos > 0 && match_buffer[cursor_pos - 1] == ' ');
    char buffer[256];

    cli_tree_node_t *help_global_tree = g_cli_local->view_tree.global_cmd_tree;

    if (has_trailing_space)
    {
        cli_tree_node_t *ctx_view = cli_tree_match_command(session->current_view->cmd_tree, match_buffer);
        cli_tree_node_t *ctx_global = help_global_tree ? cli_tree_match_command(help_global_tree, match_buffer) : NULL;
        if (ctx_view || ctx_global)
        {
            if (ctx_view)
            {
                cli_tree_print_help(ctx_view, out);
            }
            if (ctx_global)
            {
                cli_tree_print_help(ctx_global, out);
            }
        }
        else
        {
            g_string_append(out, "Error: Invalid command.\r\n");
        }
        return;
    }

    cli_tree_node_t *matches[50];
    uint32_t num_matches = cli_tree_match_command_get_matches_dual(session->current_view->cmd_tree, help_global_tree,
                                                                   match_buffer, matches, 50);
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
                    g_string_append(out, buffer);
                }
            }
        }
        else if (has_argument)
        {
            cli_tree_node_t *arg = matches[0];
            if (arg->param_type && arg->param_type->type == PARAM_TYPE_DYNAMIC)
            {
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
                if (prefix_len == 0 || strncmp(type_display + 1, prefix, prefix_len) == 0)
                {
                    snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", type_display,
                             arg->description ? arg->description : "");
                    g_string_append(out, buffer);
                }

                char **candidates = cfg_query_dynamic_candidates(arg);
                if (candidates)
                {
                    for (uint32_t ci = 0; candidates[ci]; ci++)
                    {
                        if (strncmp(candidates[ci], prefix, prefix_len) == 0)
                        {
                            snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", candidates[ci],
                                     arg->description ? arg->description : "");
                            g_string_append(out, buffer);
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
                g_string_append(out, buffer);
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
            if (session->current_view->cmd_tree)
            {
                cli_tree_print_help(session->current_view->cmd_tree, out);
            }
            if (help_global_tree)
            {
                cli_tree_print_help(help_global_tree, out);
            }
        }
        else
        {
            g_string_append(out, "Error: Invalid command.\r\n");
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

// Destroy a client session
void cli_session_destroy(cli_session_t *session)
{
    if (!session)
    {
        return;
    }

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

    if (session->out)
    {
        g_string_free(session->out, TRUE);
        session->out = NULL;
    }
    if (session->access_out_pending)
    {
        g_string_free(session->access_out_pending, TRUE);
        session->access_out_pending = NULL;
    }

    g_free(session);
}
