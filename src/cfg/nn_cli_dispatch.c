//
// Created by jhb on 1/25/26.
// CLI command dispatch - TLV message packing and module dispatch
//

#include "nn_cli_dispatch.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "nn_cfg.h"
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
        uint32_t elem_id_be = GUINT32_TO_BE(elem->element_id);
        memcpy(ptr, &elem_id_be, NN_CFG_TLV_ELEMENT_ID_SIZE);
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
int nn_cli_dispatch_to_module(nn_cli_match_result_t *result, uint32_t client_fd, nn_cli_session_t *session)
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

    // Create CLI message (sender_id and request_id will be set by nn_dev_pubsub_query)
    nn_dev_message_t *msg = nn_dev_message_create(NN_CFG_MSG_TYPE_CLI, 0, 0, msg_data, msg_len, g_free);
    if (!msg)
    {
        g_free(msg_data);
        return NN_ERRCODE_FAIL;
    }

    // Use synchronous query to wait for response
    printf("[dispatch] Sending query to module 0x%08X...\n", result->module_id);
    nn_dev_message_t *response =
        nn_dev_pubsub_query(NN_DEV_MODULE_ID_CFG, NN_DEV_EVENT_CFG, result->module_id, msg, 5000);

    // Free original message (it was cloned or used)
    nn_dev_message_free(msg);

    nn_cli_view_node_t *view = NULL;

    if (response)
    {
        if (response->msg_type == NN_CFG_MSG_TYPE_CLI_VIEW_CHG)
        {
            char module_prompt[NN_CFG_CLI_MAX_PROMPT_LEN] = {0};

            if (response->data && response->data_len > 0)
            {
                NN_CFG_TLV_GET_STRING(response->data, NN_CFG_CLI_MAX_PROMPT_LEN, module_prompt, sizeof(module_prompt));
            }

            if (result->final_node != NULL)
            {
                view = nn_cli_view_find_by_id(g_view_tree.root, result->final_node->view_id);
            }

            // Update prompt: use module-filled prompt if available, otherwise use view template
            if (module_prompt[0] != '\0' && view != NULL)
            {
                session->current_view = view;
                update_prompt_from_template(session, module_prompt);
            }
        }

        nn_dev_message_free(response);
        return NN_ERRCODE_SUCCESS;
    }
    else
    {
        nn_cfg_send_message(client_fd, "Error: Module timed out or failed to respond.\r\n");
        return NN_ERRCODE_FAIL;
    }
}
