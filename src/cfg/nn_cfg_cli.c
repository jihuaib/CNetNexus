/**
 * @file   nn_cfg_cli.c
 * @brief  CFG 模块 CLI 命令处理，处理 show、hostname 等核心命令
 * @author jhb
 * @date   2026/01/22
 */
#include "nn_cfg_cli.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "nn_cfg_main.h"
#include "nn_cli_dispatch.h"
#include "nn_cli_handler.h"
#include "nn_cli_tree.h"
#include "nn_dev.h"
#include "nn_errcode.h"

// ============================================================================
// Group Dispatch Table
// ============================================================================
typedef int (*nn_cfg_cli_cmd_group_handler_t)(nn_cfg_tlv_parser_t parser, nn_cfg_cli_out_t *cfg_out,
                                              nn_cfg_cli_resp_out_t *resp_out);

typedef struct nn_cfg_cli_cmd_group_dispatch
{
    uint32_t group_id;
    nn_cfg_cli_cmd_group_handler_t handler;
} nn_cfg_cli_cmd_group_dispatch_t;

int nn_cfg_cli_cmd_group_show(nn_cfg_tlv_parser_t parser, nn_cfg_cli_out_t *cfg_out, nn_cfg_cli_resp_out_t *resp_out);
int nn_cfg_cli_cmd_group_op(nn_cfg_tlv_parser_t parser, nn_cfg_cli_out_t *cfg_out, nn_cfg_cli_resp_out_t *resp_out);

static const nn_cfg_cli_cmd_group_dispatch_t g_nn_cfg_cli_cmd_group_dispatch[] = {
    {NN_CFG_CLI_GROUP_ID_SHOW, nn_cfg_cli_cmd_group_show},
    {NN_CFG_CLI_GROUP_ID_OP, nn_cfg_cli_cmd_group_op},
};

#define NN_CFG_CLI_CMD_GROUP_DISPATCH_COUNT                                                                            \
    (sizeof(g_nn_cfg_cli_cmd_group_dispatch) / sizeof(g_nn_cfg_cli_cmd_group_dispatch[0]))

typedef int (*nn_cfg_cli_cmd_resp_resp_t)(nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out,
                                          nn_cfg_cli_resp_out_t *resp_out);

typedef struct nn_cfg_cli_cmd_resp_dispatch
{
    uint32_t group_id;
    nn_cfg_cli_cmd_resp_resp_t handler;
} nn_cfg_cli_cmd_resp_dispatch_t;

int nn_cfg_cli_cmd_group_resp_show(nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out,
                                   nn_cfg_cli_resp_out_t *resp_out);
int nn_cfg_cli_cmd_group_resp_op(nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out,
                                 nn_cfg_cli_resp_out_t *resp_out);
void cmd_show_cli_history(nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out, nn_cfg_cli_resp_out_t *resp_out);

static const nn_cfg_cli_cmd_resp_dispatch_t g_nn_cfg_cli_cmd_resp_dispatch[] = {
    {NN_CFG_CLI_GROUP_ID_SHOW, nn_cfg_cli_cmd_group_resp_show},
    {NN_CFG_CLI_GROUP_ID_OP, nn_cfg_cli_cmd_group_resp_op},
};

#define NN_CFG_CLI_CMD_GROUP_RESP_DISPATCH_COUNT                                                                       \
    (sizeof(g_nn_cfg_cli_cmd_resp_dispatch) / sizeof(g_nn_cfg_cli_cmd_resp_dispatch[0]))

static int nn_cli_cfg_dispatch_by_group_id(uint32_t group_id, nn_cfg_tlv_parser_t parser, nn_cfg_cli_out_t *cfg_out,
                                           nn_cfg_cli_resp_out_t *resp_out)
{
    for (size_t i = 0; i < NN_CFG_CLI_CMD_GROUP_DISPATCH_COUNT; i++)
    {
        if (g_nn_cfg_cli_cmd_group_dispatch[i].group_id == group_id)
        {
            printf("[cfg_cli] Dispatching to group (group_id=%u)\n", group_id);
            return g_nn_cfg_cli_cmd_group_dispatch[i].handler(parser, cfg_out, resp_out);
        }
    }

    printf("[cfg_cli] Error: Unknown group_id: %u\n", group_id);
    snprintf(resp_out->message, sizeof(resp_out->message), "Error: Unknown command group.\r\n");
    resp_out->success = 0;
    return NN_ERRCODE_FAIL;
}

static void nn_cfg_cli_send_response(nn_cli_session_t *session, nn_cfg_cli_out_t *cfg_out,
                                     nn_cfg_cli_resp_out_t *resp_out)
{
    // Collect output with batch support - loop until no more data
    GString *full_output = g_string_new("");

    do
    {
        // Reset message buffer for each batch
        resp_out->message[0] = '\0';
        resp_out->has_more = 0;

        for (size_t i = 0; i < NN_CFG_CLI_CMD_GROUP_RESP_DISPATCH_COUNT; i++)
        {
            if (g_nn_cfg_cli_cmd_resp_dispatch[i].group_id == cfg_out->group_id)
            {
                printf("[cfg_cli] Dispatching resp to group (group_id=%u, offset=%u)\n", cfg_out->group_id,
                       resp_out->batch_offset);
                (void)g_nn_cfg_cli_cmd_resp_dispatch[i].handler(session, cfg_out, resp_out);
            }
        }

        if (resp_out->message[0] != '\0')
        {
            g_string_append(full_output, resp_out->message);
        }
    } while (resp_out->has_more);

    // Send accumulated output through pager
    if (full_output->len > 0)
    {
        nn_cli_pager_output(session, full_output->str);
    }
    g_string_free(full_output, TRUE);
}

int nn_cfg_cli_handle(nn_cli_match_result_t *result, nn_cli_session_t *session)
{
    if (!result || result->module_id == 0 || !session)
    {
        return NN_ERRCODE_FAIL;
    }

    // Pack TLV message
    uint32_t msg_len = 0;
    uint8_t *msg_data = nn_cli_dispatch_pack_tlv(result, &msg_len);
    if (!msg_data)
    {
        return NN_ERRCODE_FAIL;
    }

    nn_cfg_cli_out_t cfg_out;
    nn_cfg_cli_resp_out_t resp_out;

    memset(&cfg_out, 0, sizeof(cfg_out));
    memset(&resp_out, 0, sizeof(resp_out));

    // Parse and dispatch command
    NN_CFG_TLV_PARSE_BEGIN(msg_data, msg_len, parser, group_id)
    {
        printf("[cfg_cli] Received CLI command (group_id=%u)\n", group_id);
        cfg_out.group_id = group_id;
        (void)nn_cli_cfg_dispatch_by_group_id(group_id, parser, &cfg_out, &resp_out);
    }
    NN_CFG_TLV_PARSE_END();

    // Send response based on cfg_out and resp_out
    nn_cfg_cli_send_response(session, &cfg_out, &resp_out);

    return NN_ERRCODE_SUCCESS;
}

int nn_cfg_cli_cmd_group_show(nn_cfg_tlv_parser_t parser, nn_cfg_cli_out_t *cfg_out, nn_cfg_cli_resp_out_t *resp_out)
{
    (void)resp_out;

    NN_CFG_TLV_FOREACH(parser, elem_id, value, len)
    {
        printf("[cfg_cli]   Element ID: %u, Length: %u\n", elem_id, len);

        switch (elem_id)
        {
            case NN_CFG_CLI_SHOW_CFG_ID_COMMON_INFO:
                cfg_out->data.cfg_show.is_common_info = TRUE;
                break;
            case NN_CFG_CLI_SHOW_CFG_ID_HISTORY:
                cfg_out->data.cfg_show.is_history = TRUE;
                break;
            default:
                printf("[cfg_cli] Unknown element ID: %u\n", elem_id);
                break;
        }
    }
    return NN_ERRCODE_SUCCESS;
}

int nn_cfg_cli_cmd_group_op(nn_cfg_tlv_parser_t parser, nn_cfg_cli_out_t *cfg_out, nn_cfg_cli_resp_out_t *resp_out)
{
    (void)resp_out;

    NN_CFG_TLV_FOREACH(parser, elem_id, value, len)
    {
        printf("[cfg_cli]   Element ID: %u, Length: %u\n", elem_id, len);

        switch (elem_id)
        {
            case NN_CFG_CLI_OP_CFG_ID_EXIT:
                cfg_out->data.cfg_op.is_exit = TRUE;
                break;

            case NN_CFG_CLI_OP_CFG_ID_CONFIG:
                cfg_out->data.cfg_op.is_config = TRUE;
                break;
            case NN_CFG_CLI_OP_CFG_ID_END:
                cfg_out->data.cfg_op.is_end = TRUE;
                break;
            default:
                printf("[cfg_cli] Unknown element ID: %u\n", elem_id);
                break;
        }
    }

    return NN_ERRCODE_SUCCESS;
}

static void print_commands_recursive(GString *out, const char *view_name, const char *prefix,
                                     nn_cli_tree_node_t *node)
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
        char module_name[NN_DEV_MODULE_NAME_MAX_LEN];
        if (nn_dev_get_module_name(node->module_id, module_name) != NN_ERRCODE_SUCCESS)
        {
            snprintf(module_name, sizeof(module_name), "unknown");
        }
        char buffer[2048];
        snprintf(buffer, sizeof(buffer), "  %-15s %-15s %s\r\n", view_name, module_name, new_prefix);
        g_string_append(out, buffer);
    }

    // Recurse into children
    for (uint32_t i = 0; i < node->num_children; i++)
    {
        print_commands_recursive(out, view_name, new_prefix, node->children[i]);
    }
}

static void print_view_commands_flat(nn_cli_view_node_t *view, GString *out)
{
    if (!view)
    {
        return;
    }

    // Print commands for this view
    if (view->cmd_tree)
    {
        for (uint32_t i = 0; i < view->cmd_tree->num_children; i++)
        {
            print_commands_recursive(out, view->view_name, "", view->cmd_tree->children[i]);
        }
    }

    // Recurse into child views
    for (uint32_t i = 0; i < view->num_children; i++)
    {
        print_view_commands_flat(view->children[i], out);
    }
}

// Helper: copy a chunk from GString to resp_out with batch tracking
// Uses resp_out->batch_offset as the GString's static storage key offset
static void cfg_cli_chunk_output(GString *full, nn_cfg_cli_resp_out_t *resp_out)
{
    uint32_t offset = resp_out->batch_offset;
    if (offset >= full->len)
    {
        resp_out->message[0] = '\0';
        resp_out->has_more = 0;
        return;
    }

    size_t remaining = full->len - offset;
    size_t chunk = remaining < (NN_CFG_CLI_MAX_RESP_LEN - 1) ? remaining : (NN_CFG_CLI_MAX_RESP_LEN - 1);
    memcpy(resp_out->message, full->str + offset, chunk);
    resp_out->message[chunk] = '\0';
    resp_out->batch_offset = offset + chunk;

    if (resp_out->batch_offset < full->len)
    {
        resp_out->has_more = 1;
    }
    else
    {
        resp_out->has_more = 0;
    }
}

// Cached full output for multi-batch show cli
static GString *g_cfg_show_cache = NULL;

// Cached full output for multi-batch history
static GString *g_cfg_history_cache = NULL;

int nn_cfg_cli_cmd_group_resp_show(nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out,
                                   nn_cfg_cli_resp_out_t *resp_out)
{
    (void)session;

    if (cfg_out->data.cfg_show.is_common_info)
    {
        // On first batch (offset == 0), generate full output to cache
        if (resp_out->batch_offset == 0)
        {
            if (g_cfg_show_cache)
            {
                g_string_free(g_cfg_show_cache, TRUE);
            }
            g_cfg_show_cache = g_string_new("");

            g_string_append(g_cfg_show_cache, "\r\nCLI Commands List:\r\n");
            g_string_append(g_cfg_show_cache, "===================\r\n");
            g_string_append(g_cfg_show_cache, "  VIEW            MODULE          COMMAND\r\n");
            g_string_append(g_cfg_show_cache, "  ----            ------          -------\r\n");

            if (g_nn_cfg_local->view_tree.root)
            {
                print_view_commands_flat(g_nn_cfg_local->view_tree.root, g_cfg_show_cache);
            }

            g_string_append(g_cfg_show_cache, "\r\n");
        }

        // Copy chunk from cache to resp_out
        cfg_cli_chunk_output(g_cfg_show_cache, resp_out);

        // Free cache when done
        if (!resp_out->has_more && g_cfg_show_cache)
        {
            g_string_free(g_cfg_show_cache, TRUE);
            g_cfg_show_cache = NULL;
        }
    }
    else if (cfg_out->data.cfg_show.is_history)
    {
        cmd_show_cli_history(session, cfg_out, resp_out);
    }

    return NN_ERRCODE_SUCCESS;
}

// Show CLI history command handler
void cmd_show_cli_history(nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out, nn_cfg_cli_resp_out_t *resp_out)
{
    (void)session;
    (void)cfg_out;

    // On first batch, generate full output to cache
    if (resp_out->batch_offset == 0)
    {
        if (g_cfg_history_cache)
        {
            g_string_free(g_cfg_history_cache, TRUE);
        }
        g_cfg_history_cache = g_string_new("");
        char buffer[512];

        pthread_mutex_lock(&g_nn_cfg_local->history_mutex);

        g_string_append(g_cfg_history_cache, "\r\n");
        g_string_append(g_cfg_history_cache, "Command History:\r\n");
        g_string_append(g_cfg_history_cache,
                        "================================================================================\r\n");
        g_string_append(g_cfg_history_cache,
                        " No  Time                Command                          Client IP\r\n");
        g_string_append(g_cfg_history_cache,
                        "--------------------------------------------------------------------------------\r\n");

        for (uint32_t i = 0; i < g_nn_cfg_local->global_history.count; i++)
        {
            const nn_cli_history_entry_t *entry = nn_cli_global_history_get_entry(
                &g_nn_cfg_local->global_history, g_nn_cfg_local->global_history.count - 1 - i);
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
        snprintf(buffer, sizeof(buffer), "Total: %u command(s)\r\n\r\n", g_nn_cfg_local->global_history.count);
        g_string_append(g_cfg_history_cache, buffer);

        pthread_mutex_unlock(&g_nn_cfg_local->history_mutex);
    }

    // Copy chunk from cache to resp_out
    cfg_cli_chunk_output(g_cfg_history_cache, resp_out);

    // Free cache when done
    if (!resp_out->has_more && g_cfg_history_cache)
    {
        g_string_free(g_cfg_history_cache, TRUE);
        g_cfg_history_cache = NULL;
    }
}

int nn_cfg_cli_cmd_group_resp_op(nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out,
                                 nn_cfg_cli_resp_out_t *resp_out)
{
    (void)resp_out;

    if (cfg_out->data.cfg_op.is_config == TRUE)
    {
        nn_cli_view_node_t *config_view =
            nn_cli_view_find_by_id(g_nn_cfg_local->view_tree.root, NN_CFG_CLI_VIEW_CONFIG);
        if (config_view)
        {
            nn_cli_prompt_push(session);
            session->current_view = config_view;
            update_prompt_from_template(session, session->current_view->prompt_template);
        }
    }

    if (cfg_out->data.cfg_op.is_end == TRUE)
    {
        nn_cli_view_node_t *config_view = nn_cli_view_find_by_id(g_nn_cfg_local->view_tree.root, NN_CFG_CLI_VIEW_USER);
        if (config_view)
        {
            session->current_view = config_view;
            // Reset prompt stack and use view template directly
            session->prompt_stack_depth = 0;
            update_prompt_from_template(session, session->current_view->prompt_template);
        }
    }

    if (cfg_out->data.cfg_op.is_exit == TRUE)
    {
        nn_cli_view_node_t *parent_view = session->current_view->parent;
        if (parent_view == NULL)
        {
            close(session->client_fd);
        }
        else
        {
            nn_cli_view_node_t *config_view =
                nn_cli_view_find_by_id(g_nn_cfg_local->view_tree.root, parent_view->view_id);
            if (config_view)
            {
                session->current_view = config_view;
                nn_cli_prompt_pop(session);
            }
        }
    }

    return NN_ERRCODE_SUCCESS;
}
