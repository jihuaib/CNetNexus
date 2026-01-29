#include "nn_dev_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_cfg.h"
#include "nn_dev_module.h"
#include "nn_dev_mq.h"
#include "nn_dev_pubsub.h"
#include "nn_errcode.h"

// ============================================================================
// Internal Helpers for show commands
// ============================================================================

static gboolean show_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    nn_dev_cli_resp_out_t *resp = (nn_dev_cli_resp_out_t *)data;
    nn_dev_module_t *module = (nn_dev_module_t *)value;

    char line[128];
    snprintf(line, sizeof(line), "  %-12u %-15s %s\r\n", module->module_id, module->name,
             module->mq ? "Initialized" : "Registered");

    strncat(resp->message, line, sizeof(resp->message) - strlen(resp->message) - 1);

    return FALSE; // Continue traversal
}

static void show_module_mq_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    nn_dev_cli_resp_out_t *resp = (nn_dev_cli_resp_out_t *)data;
    nn_dev_pubsub_subscriber_t *sub = (nn_dev_pubsub_subscriber_t *)value;

    char line[128];
    uint32_t mq_len = 0;
    if (sub->mq && sub->mq->message_queue)
    {
        mq_len = g_queue_get_length(sub->mq->message_queue);
    }

    snprintf(line, sizeof(line), "  %-12u %-10d %-10u\r\n", sub->module_id, sub->eventfd, mq_len);

    strncat(resp->message, line, sizeof(resp->message) - strlen(resp->message) - 1);
}

// ============================================================================
// Group Dispatch Table
// ============================================================================
typedef int (*nn_dev_group_handler_t)(nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out,
                                      nn_dev_cli_resp_out_t *resp_out);

typedef struct nn_dev_group_dispatch
{
    uint32_t group_id;
    nn_dev_group_handler_t handler;
} nn_dev_group_dispatch_t;

static int handle_show_module(nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out, nn_dev_cli_resp_out_t *resp_out);
static int handle_show_module_mq(nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out,
                                 nn_dev_cli_resp_out_t *resp_out);
static int handle_show_version(nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out, nn_dev_cli_resp_out_t *resp_out);
static int handle_sysname(nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out, nn_dev_cli_resp_out_t *resp_out);

static const nn_dev_group_dispatch_t g_dev_group_dispatch[] = {
    {NN_DEV_CLI_GROUP_ID_SHOW_VERSION, handle_show_version},
    {NN_DEV_CLI_GROUP_ID_SHOW_MODULE, handle_show_module},
    {NN_DEV_CLI_GROUP_ID_SHOW_MODULE_MQ, handle_show_module_mq},
    {NN_DEV_CLI_GROUP_ID_SYSNAME, handle_sysname},
};

#define DEV_GROUP_DISPATCH_COUNT (sizeof(g_dev_group_dispatch) / sizeof(g_dev_group_dispatch[0]))

typedef int (*nn_dev_cfg_resp_t)(nn_dev_message_t *msg, const nn_dev_cli_out_t *cfg_out,
                                 const nn_dev_cli_resp_out_t *resp_out);

typedef struct nn_dev_cli_resp_dispatch
{
    uint32_t group_id;
    nn_dev_cfg_resp_t handler;
} nn_dev_cli_resp_dispatch_t;

static int handle_default_resp(nn_dev_message_t *msg, const nn_dev_cli_out_t *cfg_out,
                               const nn_dev_cli_resp_out_t *resp_out);

static const nn_dev_cli_resp_dispatch_t g_nn_dev_cfg_resp_dispatch[] = {
    {NN_DEV_CLI_GROUP_ID_SHOW_VERSION, handle_default_resp},
    {NN_DEV_CLI_GROUP_ID_SHOW_MODULE, handle_default_resp},
    {NN_DEV_CLI_GROUP_ID_SHOW_MODULE_MQ, handle_default_resp},
    {NN_DEV_CLI_GROUP_ID_SYSNAME, handle_default_resp},
};

#define NN_DEV_CFG_RESP_DISPATCH_COUNT (sizeof(g_nn_dev_cfg_resp_dispatch) / sizeof(g_nn_dev_cfg_resp_dispatch[0]))

// ============================================================================
// Command Handlers
// ============================================================================

static int handle_show_module(nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out, nn_dev_cli_resp_out_t *resp_out)
{
    (void)parser;
    (void)cfg_out;

    snprintf(resp_out->message, sizeof(resp_out->message),
             "\r\nRegistered Modules:\r\n"
             "  %-12s %-15s %s\r\n"
             "  -----------------------------------------\r\n",
             "ID", "Name", "Status");

    nn_dev_module_foreach(show_module_callback, resp_out);

    strncat(resp_out->message, "\r\n", sizeof(resp_out->message) - strlen(resp_out->message) - 1);
    resp_out->success = 1;
    return NN_ERRCODE_SUCCESS;
}

static int handle_show_module_mq(nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out, nn_dev_cli_resp_out_t *resp_out)
{
    (void)parser;
    (void)cfg_out;

    snprintf(resp_out->message, sizeof(resp_out->message),
             "\r\nModule Message Queues:\r\n"
             "  %-12s %-10s %-10s\r\n"
             "  -----------------------------------------\r\n",
             "Module ID", "EventFD", "Pending");

    nn_dev_pubsub_foreach_subscriber(show_module_mq_callback, resp_out);

    strncat(resp_out->message, "\r\n", sizeof(resp_out->message) - strlen(resp_out->message) - 1);
    resp_out->success = 1;
    return NN_ERRCODE_SUCCESS;
}

static int handle_show_version(nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out, nn_dev_cli_resp_out_t *resp_out)
{
    (void)parser;
    (void)cfg_out;
    snprintf(resp_out->message, sizeof(resp_out->message), "NetNexus Version 1.0.0\r\nBuild Time: %s %s\r\n", __DATE__,
             __TIME__);
    resp_out->success = 1;
    return NN_ERRCODE_SUCCESS;
}

static int handle_sysname(nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out, nn_dev_cli_resp_out_t *resp_out)
{
    (void)parser;
    (void)cfg_out;
    snprintf(resp_out->message, sizeof(resp_out->message), "Command 'sysname' not yet implemented in dev module.\r\n");
    resp_out->success = 1;
    return NN_ERRCODE_SUCCESS;
}

// ============================================================================
// Dispatch logic
// ============================================================================

static int dispatch_by_group_id(uint32_t group_id, nn_cfg_tlv_parser_t parser, nn_dev_cli_out_t *cfg_out,
                                nn_dev_cli_resp_out_t *resp_out)
{
    for (size_t i = 0; i < DEV_GROUP_DISPATCH_COUNT; i++)
    {
        if (g_dev_group_dispatch[i].group_id == group_id)
        {
            printf("[dev_cli] Dispatching to group (group_id=%u)\n", group_id);
            return g_dev_group_dispatch[i].handler(parser, cfg_out, resp_out);
        }
    }

    printf("[dev_cli] Error: Unknown group_id: %u\n", group_id);
    snprintf(resp_out->message, sizeof(resp_out->message), "Dev Error: Unknown command group %u.\r\n", group_id);
    resp_out->success = 0;
    return NN_ERRCODE_FAIL;
}

static int handle_default_resp(nn_dev_message_t *msg, const nn_dev_cli_out_t *cfg_out,
                               const nn_dev_cli_resp_out_t *resp_out)
{
    (void)cfg_out;

    char *resp_data = g_strdup(resp_out->message);
    nn_dev_message_t *resp_msg = nn_dev_message_create(NN_CFG_MSG_TYPE_CLI_RESP, NN_DEV_MODULE_ID_DEV, msg->request_id,
                                                       resp_data, strlen(resp_data) + 1, g_free);

    if (resp_msg)
    {
        nn_dev_pubsub_send_response(msg->sender_id, resp_msg);
        nn_dev_message_free(resp_msg);
    }

    return NN_ERRCODE_SUCCESS;
}

static void nn_dev_cli_send_response(nn_dev_message_t *msg, const nn_dev_cli_out_t *cfg_out,
                                     const nn_dev_cli_resp_out_t *resp_out)
{
    if (msg->sender_id == 0)
    {
        return; // No sender to respond to
    }

    for (size_t i = 0; i < NN_DEV_CFG_RESP_DISPATCH_COUNT; i++)
    {
        if (g_nn_dev_cfg_resp_dispatch[i].group_id == cfg_out->group_id)
        {
            printf("[dev_cli] Dispatching resp to group (group_id=%u)\n", cfg_out->group_id);
            (void)g_nn_dev_cfg_resp_dispatch[i].handler(msg, cfg_out, resp_out);
        }
    }
}

int nn_dev_cli_handle_message(nn_dev_message_t *msg)
{
    if (!msg || !msg->data)
    {
        return NN_ERRCODE_FAIL;
    }

    nn_dev_cli_out_t cfg_out;
    nn_dev_cli_resp_out_t resp_out;
    memset(&cfg_out, 0, sizeof(cfg_out));
    memset(&resp_out, 0, sizeof(resp_out));

    int result = NN_ERRCODE_FAIL;

    NN_CFG_TLV_PARSE_BEGIN(msg->data, msg->data_len, parser, group_id)
    {
        printf("[dev_cli] Received CLI command (group_id=%u)\n", group_id);
        cfg_out.group_id = group_id;
        result = dispatch_by_group_id(group_id, parser, &cfg_out, &resp_out);
    }
    NN_CFG_TLV_PARSE_END();

    // Send response back
    nn_dev_cli_send_response(msg, &cfg_out, &resp_out);

    return result;
}
