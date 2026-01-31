/**
 * @file   nn_bgp_cli.c
 * @brief  BGP 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#include "nn_bgp_cli.h"

#include <stdio.h>
#include <string.h>

#include "nn_cfg.h"
#include "nn_db.h"
#include "nn_dev.h"
#include "nn_errcode.h"

// ============================================================================
// Group Dispatch Table
// ============================================================================
typedef int (*nn_bgp_group_handler_t)(nn_cfg_tlv_parser_t parser, nn_bgp_cli_out_t *cfg_out,
                                      nn_bgp_cli_resp_out_t *resp_out);

typedef struct nn_bgp_group_dispatch
{
    uint32_t group_id;
    nn_bgp_group_handler_t handler;
} nn_bgp_group_dispatch_t;

int handle_bgp_config(nn_cfg_tlv_parser_t parser, nn_bgp_cli_out_t *cfg_out, nn_bgp_cli_resp_out_t *resp_out);
int handle_show_bgp(nn_cfg_tlv_parser_t parser, nn_bgp_cli_out_t *cfg_out, nn_bgp_cli_resp_out_t *resp_out);

static const nn_bgp_group_dispatch_t g_bgp_group_dispatch[] = {
    {NN_BGP_CLI_GROUP_ID_BGP, handle_bgp_config},
    {NN_BGP_CLI_GROUP_ID_SHOW, handle_show_bgp},
};

#define BGP_GROUP_DISPATCH_COUNT (sizeof(g_bgp_group_dispatch) / sizeof(g_bgp_group_dispatch[0]))

typedef int (*nn_bgp_cfg_resp_t)(nn_dev_message_t *msg, const nn_bgp_cli_out_t *cfg_out,
                                 const nn_bgp_cli_resp_out_t *resp_out);

typedef struct nn_bgp_cli_resp_dispatch
{
    uint32_t group_id;
    nn_bgp_cfg_resp_t handler;
} nn_bgp_cli_resp_dispatch_t;

int handle_bgp_config_resp(nn_dev_message_t *msg, const nn_bgp_cli_out_t *cfg_out,
                           const nn_bgp_cli_resp_out_t *resp_out);
int handle_show_bgp_resp(nn_dev_message_t *msg, const nn_bgp_cli_out_t *cfg_out,
                          const nn_bgp_cli_resp_out_t *resp_out);

static const nn_bgp_cli_resp_dispatch_t g_nn_bgp_cfg_resp_dispatch[] = {
    {NN_BGP_CLI_GROUP_ID_BGP, handle_bgp_config_resp},
    {NN_BGP_CLI_GROUP_ID_SHOW, handle_show_bgp_resp},
};

#define NN_BGP_CFG_RESP_DISPATCH_COUNT (sizeof(g_nn_bgp_cfg_resp_dispatch) / sizeof(g_nn_bgp_cfg_resp_dispatch[0]))

/**
 * @brief Handle "bgp <as-number>" command (group_id = 1)
 */
int handle_bgp_config(nn_cfg_tlv_parser_t parser, nn_bgp_cli_out_t *cfg_out, nn_bgp_cli_resp_out_t *resp_out)
{
    uint32_t as_number = 0;
    int has_as_number = 0;

    // Parse TLV elements
    NN_CFG_TLV_FOREACH(parser, cfg_id, value, len)
    {
        printf("[bgp_cfg]   CFG ID: %u, Length: %u\n", cfg_id, len);

        switch (cfg_id)
        {
            case NN_BGP_CLI_BGP_CFG_ID_BGP_NO:
            {
                printf("[bgp_cfg]     no:\n");
                cfg_out->data.bgp.no = TRUE;
                break;
            }
            case NN_BGP_CLI_BGP_CFG_ID_BGP_AS:
                NN_CFG_TLV_GET_UINT32(value, len, as_number);
                has_as_number = 1;
                printf("[bgp_cfg]     AS Number: %u\n", as_number);
                cfg_out->data.bgp.as_number = as_number;
                break;

            default:
                printf("[bgp_cfg]     Unknown CFG ID: %u\n", cfg_id);
                break;
        }
    }

    gboolean no = cfg_out->data.bgp.no;

    // 删除场景
    if (no == TRUE)
    {
        if (has_as_number)
        {
            char where[64];
            snprintf(where, sizeof(where), "as_number = %u", as_number);
            int rows = nn_db_delete("bgp_db", "bgp_protocol", where);
            snprintf(resp_out->message, sizeof(resp_out->message), "BGP: AS %u deleted (%d row).\r\n", as_number,
                     rows > 0 ? rows : 0);
        }
        else
        {
            int rows = nn_db_delete("bgp_db", "bgp_protocol", NULL);
            snprintf(resp_out->message, sizeof(resp_out->message), "BGP: All configuration deleted (%d row).\r\n",
                     rows > 0 ? rows : 0);
        }
        resp_out->success = 1;
        return NN_ERRCODE_SUCCESS;
    }

    // 配置场景：必须有 as_number
    if (!has_as_number)
    {
        snprintf(resp_out->message, sizeof(resp_out->message), "BGP Error: Missing required AS number parameter.\r\n");
        resp_out->success = 0;
        return NN_ERRCODE_FAIL;
    }

    // 检查是否已有配置
    gboolean exists = FALSE;
    int ret = nn_db_exists("bgp_db", "bgp_protocol", NULL, &exists);
    if (ret != NN_ERRCODE_SUCCESS)
    {
        snprintf(resp_out->message, sizeof(resp_out->message), "BGP Error: Database query failed.\r\n");
        resp_out->success = 0;
        return NN_ERRCODE_FAIL;
    }

    const char *field_names[] = {"as_number"};
    nn_db_value_t values[] = {nn_db_value_int((int64_t)as_number)};

    if (exists)
    {
        // 更新现有配置
        int rows = nn_db_update("bgp_db", "bgp_protocol", field_names, values, 1, NULL);
        if (rows < 0)
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "BGP Error: Failed to update configuration.\r\n");
            resp_out->success = 0;
            return NN_ERRCODE_FAIL;
        }
        printf("[bgp_cli] Updated BGP AS number to %u\n", as_number);
    }
    else
    {
        // 插入新配置
        ret = nn_db_insert("bgp_db", "bgp_protocol", field_names, values, 1);
        if (ret != NN_ERRCODE_SUCCESS)
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "BGP Error: Failed to insert configuration.\r\n");
            resp_out->success = 0;
            return NN_ERRCODE_FAIL;
        }
        printf("[bgp_cli] Inserted BGP AS number %u\n", as_number);
    }

    snprintf(resp_out->message, sizeof(resp_out->message), "BGP: AS %u configured.\r\n", as_number);
    resp_out->success = 1;

    return NN_ERRCODE_SUCCESS;
}

/**
 * @brief 处理 "show bgp" 命令 (group_id = 2)
 */
int handle_show_bgp(nn_cfg_tlv_parser_t parser, nn_bgp_cli_out_t *cfg_out, nn_bgp_cli_resp_out_t *resp_out)
{
    (void)parser;
    (void)cfg_out;

    nn_db_result_t *result = NULL;
    int ret = nn_db_query("bgp_db", "bgp_protocol", NULL, 0, NULL, &result);
    if (ret != NN_ERRCODE_SUCCESS)
    {
        snprintf(resp_out->message, sizeof(resp_out->message), "BGP Error: Database query failed.\r\n");
        resp_out->success = 0;
        return NN_ERRCODE_FAIL;
    }

    if (result->num_rows == 0)
    {
        snprintf(resp_out->message, sizeof(resp_out->message), "No BGP configuration.\r\n");
        nn_db_result_free(result);
        resp_out->success = 1;
        return NN_ERRCODE_SUCCESS;
    }

    int offset = 0;
    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        nn_db_row_t *row = result->rows[i];
        for (uint32_t j = 0; j < row->num_fields; j++)
        {
            if (strcmp(row->field_names[j], "as_number") == 0 && row->values[j].type == NN_DB_TYPE_INTEGER)
            {
                offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                                   "BGP AS Number: %ld\r\n", row->values[j].data.i64);
            }
        }
    }

    nn_db_result_free(result);
    resp_out->success = 1;
    return NN_ERRCODE_SUCCESS;
}

/**
 * @brief 发送 show bgp 命令响应
 */
int handle_show_bgp_resp(nn_dev_message_t *msg, const nn_bgp_cli_out_t *cfg_out, const nn_bgp_cli_resp_out_t *resp_out)
{
    (void)cfg_out;

    char *resp_data = g_strdup(resp_out->message);
    if (!resp_data)
    {
        return NN_ERRCODE_FAIL;
    }

    nn_dev_message_t *resp =
        nn_dev_message_create(NN_CFG_MSG_TYPE_CLI_RESP, NN_DEV_MODULE_ID_BGP, msg->request_id, resp_data,
                              strlen(resp_data) + 1, g_free);
    if (resp)
    {
        nn_dev_pubsub_send_response(msg->sender_id, resp);
        nn_dev_message_free(resp);
    }

    return NN_ERRCODE_SUCCESS;
}

/**
 * @brief Dispatch command to handler by group_id
 */
static int dispatch_by_group_id(uint32_t group_id, nn_cfg_tlv_parser_t parser, nn_bgp_cli_out_t *cfg_out,
                                nn_bgp_cli_resp_out_t *resp_out)
{
    for (size_t i = 0; i < BGP_GROUP_DISPATCH_COUNT; i++)
    {
        if (g_bgp_group_dispatch[i].group_id == group_id)
        {
            printf("[bgp_cfg] Dispatching to group (group_id=%u)\n", group_id);
            return g_bgp_group_dispatch[i].handler(parser, cfg_out, resp_out);
        }
    }

    printf("[bgp_cfg] Error: Unknown group_id: %u\n", group_id);
    snprintf(resp_out->message, sizeof(resp_out->message), "BGP Error: Unknown command group.\r\n");
    resp_out->success = 0;
    return NN_ERRCODE_FAIL;
}

int handle_bgp_config_resp_common(nn_dev_message_t *msg, const nn_bgp_cli_out_t *cfg_out,
                                  const nn_bgp_cli_resp_out_t *resp_out)
{
    (void)cfg_out;
    (void)resp_out;

    nn_dev_message_t *resp =
        nn_dev_message_create(NN_CFG_MSG_TYPE_CLI_RESP, NN_DEV_MODULE_ID_BGP, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        nn_dev_pubsub_send_response(msg->sender_id, resp);
        nn_dev_message_free(resp);
    }

    return NN_ERRCODE_SUCCESS;
}

int handle_bgp_config_resp(nn_dev_message_t *msg, const nn_bgp_cli_out_t *cfg_out,
                           const nn_bgp_cli_resp_out_t *resp_out)
{
    char view_name[NN_CFG_CLI_MAX_VIEW_LEN];

    view_name[0] = '\0';

    if (cfg_out->data.bgp.no == FALSE)
    {
        char out_prompt[NN_CFG_CLI_MAX_PROMPT_LEN];

        out_prompt[0] = '\0';

        (void)resp_out;

        int ret = nn_cfg_get_view_prompt_template(NN_CFG_CLI_VIEW_BGP, view_name);
        if (ret != NN_ERRCODE_SUCCESS)
        {
            return NN_ERRCODE_FAIL;
        }

        snprintf(out_prompt, NN_CFG_CLI_MAX_PROMPT_LEN, view_name, cfg_out->data.bgp.as_number);
        char *msg_out = g_malloc0(NN_CFG_CLI_MAX_PROMPT_LEN);
        memcpy(msg_out, out_prompt, NN_CFG_CLI_MAX_PROMPT_LEN);

        nn_dev_message_t *resp = nn_dev_message_create(NN_CFG_MSG_TYPE_CLI_VIEW_CHG, NN_DEV_MODULE_ID_BGP,
                                                       msg->request_id, msg_out, NN_CFG_CLI_MAX_PROMPT_LEN, g_free);
        if (resp)
        {
            nn_dev_pubsub_send_response(msg->sender_id, resp);
            nn_dev_message_free(resp);
        }
    }
    else
    {
        return handle_bgp_config_resp_common(msg, cfg_out, resp_out);
    }

    return NN_ERRCODE_SUCCESS;
}

/**
 * @brief Send response back to sender based on cfg_out and resp_out
 */
static void nn_bgp_cfg_send_response(nn_dev_message_t *msg, const nn_bgp_cli_out_t *cfg_out,
                                     const nn_bgp_cli_resp_out_t *resp_out)
{
    if (msg->sender_id == 0)
    {
        return; // No sender to respond to
    }

    for (size_t i = 0; i < NN_BGP_CFG_RESP_DISPATCH_COUNT; i++)
    {
        if (g_nn_bgp_cfg_resp_dispatch[i].group_id == cfg_out->group_id)
        {
            printf("[bgp_cfg] Dispatching resp to group (group_id=%u)\n", cfg_out->group_id);
            (void)g_nn_bgp_cfg_resp_dispatch[i].handler(msg, cfg_out, resp_out);
        }
    }
}

// ============================================================================
// Main Entry Point
// ============================================================================

/**
 * @brief Parse and dispatch BGP CLI command from TLV message
 */
int nn_bgp_cli_handle_continue(nn_dev_message_t *msg)
{
    // No batch output pending - send empty final response
    char *resp_data = g_strdup("");
    nn_dev_message_t *resp_msg =
        nn_dev_message_create(NN_CFG_MSG_TYPE_CLI_RESP, NN_DEV_MODULE_ID_BGP, msg->request_id,
                             resp_data, strlen(resp_data) + 1, g_free);
    if (resp_msg)
    {
        nn_dev_pubsub_send_response(msg->sender_id, resp_msg);
        nn_dev_message_free(resp_msg);
    }
    return NN_ERRCODE_SUCCESS;
}

int nn_bgp_cli_handle_message(nn_dev_message_t *msg)
{
    if (!msg || !msg->data)
    {
        return NN_ERRCODE_FAIL;
    }

    // Initialize output structures
    nn_bgp_cli_out_t cfg_out;
    nn_bgp_cli_resp_out_t resp_out;

    memset(&cfg_out, 0, sizeof(cfg_out));
    memset(&resp_out, 0, sizeof(resp_out));

    int result = NN_ERRCODE_FAIL;

    // Parse and dispatch command
    NN_CFG_TLV_PARSE_BEGIN(msg->data, msg->data_len, parser, group_id)
    {
        printf("[bgp_cfg] Received CLI command (group_id=%u)\n", group_id);
        cfg_out.group_id = group_id;
        result = dispatch_by_group_id(group_id, parser, &cfg_out, &resp_out);
    }
    NN_CFG_TLV_PARSE_END();

    // Send response based on cfg_out and resp_out
    nn_bgp_cfg_send_response(msg, &cfg_out, &resp_out);

    return result;
}
