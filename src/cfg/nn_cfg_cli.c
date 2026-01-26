//
// Created by j31397 on 2026/1/26.
//

#include "nn_cfg_cli.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

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
    {NN_CFG_CLI_GROUP_ID_SHOW_CLI, nn_cfg_cli_cmd_group_show},
    {NN_CFG_CLI_GROUP_ID_OP, nn_cfg_cli_cmd_group_op},
};

#define NN_CFG_CLI_CMD_GROUP_DISPATCH_COUNT (sizeof(g_nn_cfg_cli_cmd_group_dispatch) / sizeof(g_nn_cfg_cli_cmd_group_dispatch[0]))

typedef int (*nn_cfg_cli_cmd_resp_resp_t)(uint32_t client_fd, nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out,
                                 const nn_cfg_cli_resp_out_t *resp_out);

typedef struct nn_cfg_cli_cmd_resp_dispatch
{
    uint32_t group_id;
    nn_cfg_cli_cmd_resp_resp_t handler;
} nn_cfg_cli_cmd_resp_dispatch_t;

int nn_cfg_cli_cmd_group_resp_show(uint32_t client_fd, nn_cli_session_t *session,const nn_cfg_cli_out_t *cfg_out, const nn_cfg_cli_resp_out_t *resp_out);
int nn_cfg_cli_cmd_group_resp_op(uint32_t client_fd, nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out, const nn_cfg_cli_resp_out_t *resp_out);

static const nn_cfg_cli_cmd_resp_dispatch_t g_nn_cfg_cli_cmd_resp_dispatch[] = {
    {NN_CFG_CLI_GROUP_ID_SHOW_CLI, nn_cfg_cli_cmd_group_resp_show},
    {NN_CFG_CLI_GROUP_ID_OP, nn_cfg_cli_cmd_group_resp_op},
};

#define NN_CFG_CLI_CMD_GROUP_RESP_DISPATCH_COUNT (sizeof(g_nn_cfg_cli_cmd_resp_dispatch) / sizeof(g_nn_cfg_cli_cmd_resp_dispatch[0]))

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

static void nn_cfg_cli_send_response(uint32_t client_fd, nn_cli_session_t *session, nn_cfg_cli_out_t *cfg_out, nn_cfg_cli_resp_out_t *resp_out)
{

    for (size_t i = 0; i < NN_CFG_CLI_CMD_GROUP_RESP_DISPATCH_COUNT; i++)
    {
        if (g_nn_cfg_cli_cmd_resp_dispatch[i].group_id == cfg_out->group_id)
        {
            printf("[cfg_cli] Dispatching resp to group (group_id=%u)\n", cfg_out->group_id);
            (void)g_nn_cfg_cli_cmd_resp_dispatch[i].handler(client_fd, session, cfg_out, resp_out);
        }
    }
}

int nn_cfg_cli_handle(nn_cli_match_result_t *result, uint32_t client_fd, nn_cli_session_t *session)
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
    nn_cfg_cli_send_response(client_fd, session, &cfg_out, &resp_out);
}

int nn_cfg_cli_cmd_group_show(nn_cfg_tlv_parser_t parser, nn_cfg_cli_out_t *cfg_out, nn_cfg_cli_resp_out_t *resp_out)
{
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
            case NN_CFG_CLI_OP_ELEM_ID_EXIT:
                cfg_out->data.cfg_op.is_exit = true;
                break;

            case NN_CFG_CLI_OP_ELEM_ID_CONFIG:
                cfg_out->data.cfg_op.is_config = true;
                break;
            case NN_CFG_CLI_OP_ELEM_ID_END:
                cfg_out->data.cfg_op.is_end = true;
                break;
            default:
                printf("[cfg_cli] Unknown element ID: %u\n", elem_id);
                break;
        }
    }

    return NN_ERRCODE_SUCCESS;
}

static void print_commands_recursive(uint32_t client_fd, const char *view_name, const char *prefix, nn_cli_tree_node_t *node)
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

    if (node->num_children == 0)
    {
        char buffer[2048];
        snprintf(buffer, sizeof(buffer), "  %-15s %-15s %s\r\n", view_name,
                  "builtin", new_prefix);
        nn_cfg_send_message(client_fd, buffer);
    }

    // Recurse into children
    for (uint32_t i = 0; i < node->num_children; i++)
    {
        print_commands_recursive(client_fd, view_name, new_prefix, node->children[i]);
    }
}

static void print_view_commands_flat(nn_cli_view_node_t *view, uint32_t client_fd)
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
            print_commands_recursive(client_fd, view->view_name, "", view->cmd_tree->children[i]);
        }
    }

    // Recurse into child views
    for (uint32_t i = 0; i < view->num_children; i++)
    {
        print_view_commands_flat(view->children[i], client_fd);
    }
}

int nn_cfg_cli_cmd_group_resp_show(uint32_t client_fd, nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out, const nn_cfg_cli_resp_out_t *resp_out)
{
    nn_cfg_send_message(client_fd, "\r\nCLI Commands List:\r\n");
    nn_cfg_send_message(client_fd, "===================\r\n");
    nn_cfg_send_message(client_fd, "  VIEW            MODULE          COMMAND\r\n");
    nn_cfg_send_message(client_fd, "  ----            ------          -------\r\n");

    if (g_view_tree.root)
    {
        print_view_commands_flat(g_view_tree.root, client_fd);
    }

    nn_cfg_send_message(client_fd, "\r\n");

    return NN_ERRCODE_SUCCESS;
}

int nn_cfg_cli_cmd_group_resp_op(uint32_t client_fd, nn_cli_session_t *session, const nn_cfg_cli_out_t *cfg_out, const nn_cfg_cli_resp_out_t *resp_out)
{
    (void)cfg_out;
    (void)resp_out;

    if (cfg_out->data.cfg_op.is_config == true)
    {
        nn_cli_view_node_t *config_view = nn_cli_view_find_by_id(g_view_tree.root, NN_CFG_CLI_VIEW_CONFIG);
        if (config_view)
        {
            session->current_view = config_view;
            update_prompt(session);
        }
    }

    if (cfg_out->data.cfg_op.is_end == true)
    {
        nn_cli_view_node_t *config_view = nn_cli_view_find_by_id(g_view_tree.root, NN_CFG_CLI_VIEW_USER);
        if (config_view)
        {
            session->current_view = config_view;
            update_prompt(session);
        }
    }

    if (cfg_out->data.cfg_op.is_exit == true)
    {
        nn_cli_view_node_t *parent_view = session->current_view->parent;
        if (parent_view == NULL)
        {
            close(client_fd);
        }
        else
        {
            nn_cli_view_node_t *config_view = nn_cli_view_find_by_id(g_view_tree.root, parent_view->view_id);
            if (config_view)
            {
                session->current_view = config_view;
                update_prompt(session);
            }
        }
    }

    return NN_ERRCODE_SUCCESS;
}
