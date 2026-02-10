/**
 * @file   cfg_cli.c
 * @brief  CFG 模块 CLI 命令处理，处理 show、exit、config 等核心命令
 * @author jhb
 * @date   2026/01/22
 */
#include "cfg_cli.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cfg_main.h"
#include "cfg_show_config.h"
#include "cli_handler.h"
#include "dev.h"
#include "errcode.h"

// ============================================================================
// 命令树打印
// ============================================================================

static void print_commands_recursive(GString *out, const char *view_name, const char *prefix, cli_tree_node_t *node)
{
    if (!node)
    {
        return;
    }

    char new_prefix[MAX_CMD_LEN];
    if (strlen(prefix) > 0)
    {
        snprintf(new_prefix, sizeof(new_prefix), "%s %s", prefix, node->name ? node->name : "");
    }
    else
    {
        strncpy(new_prefix, node->name ? node->name : "", sizeof(new_prefix) - 1);
        new_prefix[sizeof(new_prefix) - 1] = '\0';
    }

    if (node->is_end_node == TRUE)
    {
        char module_name[DEV_MODULE_NAME_MAX_LEN];
        if (dev_get_module_name(node->module_id, module_name) != ERRCODE_SUCCESS)
        {
            snprintf(module_name, sizeof(module_name), "unknown");
        }
        char buffer[2048];
        snprintf(buffer, sizeof(buffer), "  %-15s %-15s %s\r\n", view_name, module_name, new_prefix);
        g_string_append(out, buffer);
    }

    for (uint32_t i = 0; i < node->num_children; i++)
    {
        print_commands_recursive(out, view_name, new_prefix, node->children[i]);
    }
}

static void print_view_commands_flat(cli_view_node_t *view, GString *out)
{
    if (!view)
    {
        return;
    }

    if (view->cmd_tree)
    {
        for (uint32_t i = 0; i < view->cmd_tree->num_children; i++)
        {
            print_commands_recursive(out, view->view_name, "", view->cmd_tree->children[i]);
        }
    }

    for (uint32_t i = 0; i < view->num_children; i++)
    {
        print_view_commands_flat(view->children[i], out);
    }
}

// ============================================================================
// 分批输出辅助
// ============================================================================

static void cfg_cli_chunk_output(GString *full, cfg_cli_resp_out_t *resp_out)
{
    uint32_t offset = resp_out->batch_offset;
    if (offset >= full->len)
    {
        resp_out->message[0] = '\0';
        resp_out->has_more = 0;
        return;
    }

    size_t remaining = full->len - offset;
    size_t chunk = remaining < (CLI_MAX_RESP_LEN - 1) ? remaining : (CLI_MAX_RESP_LEN - 1);
    memcpy(resp_out->message, full->str + offset, chunk);
    resp_out->message[chunk] = '\0';
    resp_out->batch_offset = offset + chunk;

    resp_out->has_more = (resp_out->batch_offset < full->len) ? 1 : 0;
}

/* 缓存 show 输出 */
static GString *g_cfg_show_cache = NULL;

/* 缓存 history 输出 */
static GString *g_cfg_history_cache = NULL;

// ============================================================================
// 按 table_name 分发的 handler
// ============================================================================

/**
 * @brief show cli command-info
 */
static void handle_show_commands(cli_session_t *session)
{
    GString *full_output = g_string_new("");

    cfg_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));

    /* 生成完整输出到缓存 */
    if (g_cfg_show_cache)
    {
        g_string_free(g_cfg_show_cache, TRUE);
    }
    g_cfg_show_cache = g_string_new("");

    g_string_append(g_cfg_show_cache, "\r\nCLI Commands List:\r\n");
    g_string_append(g_cfg_show_cache, "===================\r\n");
    g_string_append(g_cfg_show_cache, "  VIEW            MODULE          COMMAND\r\n");
    g_string_append(g_cfg_show_cache, "  ----            ------          -------\r\n");

    if (g_cfg_local->view_tree.root)
    {
        print_view_commands_flat(g_cfg_local->view_tree.root, g_cfg_show_cache);
    }

    g_string_append(g_cfg_show_cache, "\r\n");

    /* 分批读取 */
    do
    {
        resp_out.message[0] = '\0';
        resp_out.has_more = 0;
        cfg_cli_chunk_output(g_cfg_show_cache, &resp_out);
        if (resp_out.message[0] != '\0')
        {
            g_string_append(full_output, resp_out.message);
        }
    } while (resp_out.has_more);

    g_string_free(g_cfg_show_cache, TRUE);
    g_cfg_show_cache = NULL;

    if (full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }
    g_string_free(full_output, TRUE);
}

/**
 * @brief show cli history
 */
static void handle_show_history(cli_session_t *session)
{
    GString *full_output = g_string_new("");
    cfg_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));

    if (g_cfg_history_cache)
    {
        g_string_free(g_cfg_history_cache, TRUE);
    }
    g_cfg_history_cache = g_string_new("");
    char buffer[512];

    pthread_mutex_lock(&g_cfg_local->history_mutex);

    g_string_append(g_cfg_history_cache, "\r\n");
    g_string_append(g_cfg_history_cache, "Command History:\r\n");
    g_string_append(g_cfg_history_cache,
                    "================================================================================\r\n");
    g_string_append(g_cfg_history_cache, " No  Time                Command                          Client IP\r\n");
    g_string_append(g_cfg_history_cache,
                    "--------------------------------------------------------------------------------\r\n");

    for (uint32_t i = 0; i < g_cfg_local->global_history.count; i++)
    {
        const cli_history_entry_t *entry =
            cli_global_history_get_entry(&g_cfg_local->global_history, g_cfg_local->global_history.count - 1 - i);
        if (entry && entry->command)
        {
            struct tm *timeinfo = localtime(&entry->timestamp);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);

            char cmd_display[33];
            if (strlen(entry->command) > 32)
            {
                strncpy(cmd_display, entry->command, 29);
                cmd_display[29] = '.';
                cmd_display[30] = '.';
                cmd_display[31] = '.';
                cmd_display[32] = '\0';
            }
            else
            {
                strcpy(cmd_display, entry->command);
            }

            snprintf(buffer, sizeof(buffer), " %-3u %-19s %-32s %-15s\r\n", i + 1, time_str, cmd_display,
                     entry->client_ip);
            g_string_append(g_cfg_history_cache, buffer);
        }
    }

    g_string_append(g_cfg_history_cache,
                    "================================================================================\r\n");
    snprintf(buffer, sizeof(buffer), "Total: %u command(s)\r\n\r\n", g_cfg_local->global_history.count);
    g_string_append(g_cfg_history_cache, buffer);

    pthread_mutex_unlock(&g_cfg_local->history_mutex);

    do
    {
        resp_out.message[0] = '\0';
        resp_out.has_more = 0;
        cfg_cli_chunk_output(g_cfg_history_cache, &resp_out);
        if (resp_out.message[0] != '\0')
        {
            g_string_append(full_output, resp_out.message);
        }
    } while (resp_out.has_more);

    g_string_free(g_cfg_history_cache, TRUE);
    g_cfg_history_cache = NULL;

    if (full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }
    g_string_free(full_output, TRUE);
}

/**
 * @brief show current-configuration
 */
static void handle_show_config(cli_session_t *session)
{
    char *config_output = cfg_renderer_show_current_configuration();
    if (config_output && config_output[0] != '\0')
    {
        cli_pager_output(session, config_output);
    }
    else
    {
        cfg_send_message(session, "No configuration found.\r\n");
    }
    g_free(config_output);
}

/**
 * @brief exit
 */
static void handle_op_exit(cli_session_t *session)
{
    cli_view_node_t *parent_view = session->current_view->parent;
    if (parent_view == NULL)
    {
        close(session->client_fd);
    }
    else
    {
        cli_view_node_t *config_view = cli_view_find_by_id(g_cfg_local->view_tree.root, parent_view->view_id);
        if (config_view)
        {
            session->current_view = config_view;
            cli_prompt_pop(session);
        }
    }
}

/**
 * @brief config
 */
static void handle_op_config(cli_session_t *session)
{
    cli_view_node_t *config_view = cli_view_find_by_id(g_cfg_local->view_tree.root, CLI_VIEW_CONFIG);
    if (config_view)
    {
        cli_prompt_push(session);
        session->current_view = config_view;
        update_prompt_from_template(session, session->current_view->prompt_template);
    }
}

/**
 * @brief end
 */
static void handle_op_end(cli_session_t *session)
{
    cli_view_node_t *user_view = cli_view_find_by_id(g_cfg_local->view_tree.root, CLI_VIEW_USER);
    if (user_view)
    {
        session->current_view = user_view;
        /* 释放所有上下文栈数据并重置 prompt 栈 */
        for (uint32_t i = 0; i < session->prompt_stack_depth; i++)
        {
            if (session->view_context_stack[i])
            {
                g_free(session->view_context_stack[i]);
                session->view_context_stack[i] = NULL;
                session->view_context_len[i] = 0;
            }
        }
        session->prompt_stack_depth = 0;
        update_prompt_from_template(session, session->current_view->prompt_template);
    }
}

// ============================================================================
// 主入口
// ============================================================================

int cfg_cli_handle(ipc_message_t *msg, cli_session_t *session)
{
    if (!msg || !msg->payload || !session)
    {
        return ERRCODE_FAIL;
    }

    cli_db_payload_parser_t parser;
    if (cli_db_payload_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        printf("[cfg_cli] 载荷解析失败\n");
        return ERRCODE_FAIL;
    }

    printf("[cfg_cli] 收到命令 (table=%s)\n", parser.table_name);

    if (strcmp(parser.table_name, "cfg_show_commands") == 0)
    {
        handle_show_commands(session);
    }
    else if (strcmp(parser.table_name, "cfg_show_history") == 0)
    {
        handle_show_history(session);
    }
    else if (strcmp(parser.table_name, "cfg_show_config") == 0)
    {
        handle_show_config(session);
    }
    else if (strcmp(parser.table_name, "cfg_op_exit") == 0)
    {
        handle_op_exit(session);
    }
    else if (strcmp(parser.table_name, "cfg_op_config") == 0)
    {
        handle_op_config(session);
    }
    else if (strcmp(parser.table_name, "cfg_op_end") == 0)
    {
        handle_op_end(session);
    }
    else
    {
        printf("[cfg_cli] 未知表名: %s\n", parser.table_name);
        cfg_send_message(session, "Error: Unknown CFG command.\r\n");
        cli_db_payload_cleanup(&parser);
        return ERRCODE_FAIL;
    }

    cli_db_payload_cleanup(&parser);
    return ERRCODE_SUCCESS;
}
