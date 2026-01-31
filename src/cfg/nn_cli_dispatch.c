/**
 * @file   nn_cli_dispatch.c
 * @brief  CLI 命令分发，TLV 消息打包和模块路由
 * @author jhb
 * @date   2026/01/22
 */
#include "nn_cli_dispatch.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "nn_cfg.h"
#include "nn_cfg_main.h"
#include "nn_cli_param_type.h"
#include "nn_dev.h"
#include "nn_errcode.h"

// Calculate total TLV buffer size needed
static uint32_t calculate_tlv_size(nn_cli_match_result_t *result)
{
    // Start with module_id
    uint32_t size = NN_CFG_TLV_GROUP_ID_SIZE;

    // Add each element: element_id (4) + length (2) + value
    for (uint32_t i = 0; i < result->num_elements; i++)
    {
        size += NN_CFG_TLV_ELEMENT_ID_SIZE + NN_CFG_TLV_LENGTH_SIZE;
        if (result->elements[i].value)
        {
            size += result->elements[i].value_len;
        }
    }

    return size;
}

// 将上下文 TLV 数据中的每个 cfg_id 加上 CONTEXT_FLAG 标记后重新打包
static uint8_t *mark_context_tlvs(const uint8_t *ctx_data, uint32_t ctx_len, uint32_t *out_len)
{
    if (!ctx_data || ctx_len == 0 || !out_len)
    {
        if (out_len)
        {
            *out_len = 0;
        }
        return NULL;
    }

    // 输出大小与输入相同（只修改 cfg_id 字段的高位）
    uint8_t *output = g_malloc(ctx_len);
    uint32_t offset = 0;
    uint32_t out_offset = 0;

    while (offset + NN_CFG_TLV_HEADER_SIZE <= ctx_len)
    {
        // 读取原始 cfg_id
        uint32_t cfg_id_be;
        memcpy(&cfg_id_be, ctx_data + offset, NN_CFG_TLV_ELEMENT_ID_SIZE);
        uint32_t cfg_id = ntohl(cfg_id_be);

        // 加上上下文标记
        uint32_t marked_id = cfg_id | NN_CFG_TLV_CONTEXT_FLAG;
        uint32_t marked_id_be = htonl(marked_id);
        memcpy(output + out_offset, &marked_id_be, NN_CFG_TLV_ELEMENT_ID_SIZE);
        out_offset += NN_CFG_TLV_ELEMENT_ID_SIZE;
        offset += NN_CFG_TLV_ELEMENT_ID_SIZE;

        // 读取长度
        uint16_t len_be;
        memcpy(&len_be, ctx_data + offset, NN_CFG_TLV_LENGTH_SIZE);
        uint16_t val_len = ntohs(len_be);

        // 复制长度字段
        memcpy(output + out_offset, ctx_data + offset, NN_CFG_TLV_LENGTH_SIZE);
        out_offset += NN_CFG_TLV_LENGTH_SIZE;
        offset += NN_CFG_TLV_LENGTH_SIZE;

        // 检查边界
        if (offset + val_len > ctx_len)
        {
            break;
        }

        // 复制值
        if (val_len > 0)
        {
            memcpy(output + out_offset, ctx_data + offset, val_len);
            out_offset += val_len;
            offset += val_len;
        }
    }

    *out_len = out_offset;
    return output;
}

// Pack match result into TLV buffer
uint8_t *nn_cli_dispatch_pack_tlv(nn_cli_match_result_t *result, uint32_t *out_len)
{
    if (!result || !out_len)
    {
        return NULL;
    }

    uint32_t total_size = calculate_tlv_size(result);
    uint8_t *buffer = g_malloc0(total_size);
    uint8_t *ptr = buffer;

    // Pack module_id (4 bytes, network byte order)
    uint32_t group_id_be = GUINT32_TO_BE(result->group_id);
    memcpy(ptr, &group_id_be, NN_CFG_TLV_GROUP_ID_SIZE);
    ptr += NN_CFG_TLV_GROUP_ID_SIZE;

    // Pack each element as TLV
    for (uint32_t i = 0; i < result->num_elements; i++)
    {
        nn_cli_match_element_t *elem = &result->elements[i];

        // Element ID (4 bytes, network byte order)
        uint32_t cfg_id_be = GUINT32_TO_BE(elem->cfg_id);
        memcpy(ptr, &cfg_id_be, NN_CFG_TLV_ELEMENT_ID_SIZE);
        ptr += NN_CFG_TLV_ELEMENT_ID_SIZE;

        // Length (2 bytes, network byte order)
        uint16_t len_be = GUINT16_TO_BE(elem->value_len);
        memcpy(ptr, &len_be, NN_CFG_TLV_LENGTH_SIZE);
        ptr += NN_CFG_TLV_LENGTH_SIZE;

        // Value (only if present) - convert based on parameter type
        if (elem->value && elem->value_len > 0)
        {
            // Convert value to binary format based on actual parameter type
            if (elem->param_type)
            {
                switch (elem->param_type->type)
                {
                    case NN_PARAM_TYPE_UINT:
                    case NN_PARAM_TYPE_INT:
                    {
                        // Convert string to uint32_t and pack in network byte order
                        char *endptr;
                        unsigned long val = strtoul(elem->value, &endptr, 10);
                        if (*endptr == '\0')
                        {
                            uint32_t val_be = GUINT32_TO_BE((uint32_t)val);
                            memcpy(ptr, &val_be, sizeof(uint32_t));
                            ptr += sizeof(uint32_t);
                        }
                        else
                        {
                            // Conversion failed, shouldn't happen with validation
                            memcpy(ptr, elem->value, elem->value_len);
                            ptr += elem->value_len;
                        }
                        break;
                    }

                    case NN_PARAM_TYPE_IPV4:
                    {
                        // Convert IPv4 string to 4-byte binary
                        struct in_addr addr;
                        if (inet_pton(AF_INET, elem->value, &addr) == 1)
                        {
                            memcpy(ptr, &addr, 4);
                            ptr += 4;
                        }
                        else
                        {
                            // Conversion failed
                            memcpy(ptr, elem->value, elem->value_len);
                            ptr += elem->value_len;
                        }
                        break;
                    }

                        // Add more types as needed (IPv6, MAC, etc.)

                    default:
                        // String or unknown type - copy as-is
                        memcpy(ptr, elem->value, elem->value_len);
                        ptr += elem->value_len;
                        break;
                }
            }
            else
            {
                // No param_type - copy as-is
                memcpy(ptr, elem->value, elem->value_len);
                ptr += elem->value_len;
            }
        }
    }

    *out_len = total_size;
    return buffer;
}

// Dispatch command to target module via pub/sub (synchronous)
int nn_cli_dispatch_to_module(nn_cli_match_result_t *result, nn_cli_session_t *session)
{
    if (!result || result->module_id == 0 || !session)
    {
        return NN_ERRCODE_FAIL;
    }

    // Pack TLV message
    uint32_t cmd_len = 0;
    uint8_t *cmd_data = nn_cli_dispatch_pack_tlv(result, &cmd_len);
    if (!cmd_data)
    {
        return NN_ERRCODE_FAIL;
    }

    // 获取当前视图上下文，追加到命令消息末尾
    uint32_t ctx_len = 0;
    const uint8_t *ctx_data = nn_cli_context_get(session, &ctx_len);

    uint8_t *msg_data = NULL;
    uint32_t msg_len = 0;

    if (ctx_data && ctx_len > 0)
    {
        // 将上下文 TLV 的 cfg_id 加上标记位后追加
        uint32_t ctx_marked_len = 0;
        uint8_t *ctx_marked = mark_context_tlvs(ctx_data, ctx_len, &ctx_marked_len);

        if (ctx_marked && ctx_marked_len > 0)
        {
            msg_len = cmd_len + ctx_marked_len;
            msg_data = g_malloc(msg_len);
            memcpy(msg_data, cmd_data, cmd_len);
            memcpy(msg_data + cmd_len, ctx_marked, ctx_marked_len);
            g_free(cmd_data);
            g_free(ctx_marked);
        }
        else
        {
            msg_data = cmd_data;
            msg_len = cmd_len;
        }
    }
    else
    {
        msg_data = cmd_data;
        msg_len = cmd_len;
    }

    // Create CLI message (sender_id and request_id will be set by nn_dev_pubsub_query)
    nn_dev_message_t *msg = nn_dev_message_create(NN_CFG_MSG_TYPE_CLI, 0, 0, msg_data, msg_len, g_free);
    if (!msg)
    {
        g_free(msg_data);
        return NN_ERRCODE_FAIL;
    }

    // Use synchronous query to wait for response, with batch support
    printf("[dispatch] Sending query to module 0x%08X...\n", result->module_id);

    GString *full_output = g_string_new("");
    nn_cli_view_node_t *view = NULL;
    int done = 0;

    while (!done)
    {
        nn_dev_message_t *response =
            nn_dev_pubsub_query(NN_DEV_MODULE_ID_CFG, NN_DEV_EVENT_CFG, result->module_id, msg, 5000);

        // Free the query message (original or continue)
        nn_dev_message_free(msg);
        msg = NULL;

        if (!response)
        {
            if (full_output->len == 0)
            {
                nn_cfg_send_message(session, "Error: Module timed out or failed to respond.\r\n");
            }
            g_string_free(full_output, TRUE);
            return NN_ERRCODE_FAIL;
        }

        if (response->msg_type == NN_CFG_MSG_TYPE_CLI_VIEW_CHG)
        {
            char module_prompt[NN_CFG_CLI_MAX_PROMPT_LEN] = {0};

            if (response->data && response->data_len > 0)
            {
                NN_CFG_TLV_GET_STRING(response->data, NN_CFG_CLI_MAX_PROMPT_LEN, module_prompt, sizeof(module_prompt));
            }

            if (result->final_node != NULL)
            {
                view = nn_cli_view_find_by_id(g_nn_cfg_local->view_tree.root, result->final_node->view_id);
            }

            if (module_prompt[0] != '\0' && view != NULL)
            {
                nn_cli_prompt_push(session);
                session->current_view = view;
                update_prompt_from_template(session, module_prompt);

                // 提取上下文 TLV（prompt 之后的剩余数据）
                if (response->data_len > NN_CFG_CLI_MAX_PROMPT_LEN)
                {
                    uint32_t ctx_len = response->data_len - NN_CFG_CLI_MAX_PROMPT_LEN;
                    const uint8_t *ctx_data = (const uint8_t *)response->data + NN_CFG_CLI_MAX_PROMPT_LEN;
                    nn_cli_context_set(session, ctx_data, ctx_len);
                    printf("[dispatch] Saved view context (%u bytes)\n", ctx_len);
                }
            }

            nn_dev_message_free(response);
            done = 1;
        }
        else if (response->msg_type == NN_CFG_MSG_TYPE_CLI_RESP)
        {
            // Final response chunk
            if (response->data)
            {
                g_string_append(full_output, response->data);
            }
            nn_dev_message_free(response);
            done = 1;
        }
        else if (response->msg_type == NN_CFG_MSG_TYPE_CLI_RESP_MORE)
        {
            // Partial response - append and request more
            if (response->data)
            {
                g_string_append(full_output, response->data);
            }
            nn_dev_message_free(response);

            // Send CONTINUE to request next batch
            msg = nn_dev_message_create(NN_CFG_MSG_TYPE_CLI_CONTINUE, 0, 0, NULL, 0, NULL);
            if (!msg)
            {
                done = 1;
            }
        }
        else
        {
            nn_dev_message_free(response);
            done = 1;
        }
    }

    // Send accumulated output through pager
    if (full_output->len > 0)
    {
        nn_cli_pager_output(session, full_output->str);
    }
    g_string_free(full_output, TRUE);

    return NN_ERRCODE_SUCCESS;
}
