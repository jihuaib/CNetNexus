#include "srv6_cli.h"

#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "errcode.h"
#include "srv6_db.h"
#include "srv6_main.h"
#include "work/srv6_worker.h"

void srv6_cli_send_response(dev_ipc_message_t *msg, uint32_t response_type, const char *text)
{
    const char *safe = text ? text : "";
    char *payload = g_strdup(safe);
    dev_ipc_message_t *resp = dev_ipc_message_create(response_type, DEV_MODULE_ID_SRV6, msg->src_module_id,
                                                     msg->request_id, payload, strlen(payload) + 1u, g_free);
    if (!resp)
    {
        g_free(payload);
        return;
    }
    (void)dev_ipc_send_response(srv6_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
}

static int srv6_cli_handle_protocol(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0u;
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }
    if (!is_no)
    {
        srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP, "");
        return ERRCODE_SUCCESS;
    }

    char error[256] = "";
    int rc = srv6_worker_delete_config(error, sizeof(error));
    if (rc != ERRCODE_SUCCESS)
    {
        char response[320];
        snprintf(response, sizeof(response), "SRV6 Error: %s\r\n", error[0] ? error : "configuration removal failed");
        srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP, response);
        return rc;
    }
    srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP_EXITING, "SRV6: configuration cleared, process exiting.\r\n");
    kill(getpid(), SIGTERM);
    return ERRCODE_SUCCESS;
}

static int srv6_cli_handle_locator(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0u;
    char name[SRV6_LOCATOR_NAME_MAX] = "";
    char prefix_text[INET6_ADDRSTRLEN] = "";
    uint32_t prefix_len = UINT32_MAX;
    uint32_t function_bits = SRV6_DEFAULT_FUNCTION_BITS;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry))
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (entry.cfg_id == 1u && text)
            {
                g_strlcpy(name, text, sizeof(name));
            }
            else if (entry.cfg_id == 2u && text)
            {
                g_strlcpy(prefix_text, text, sizeof(prefix_text));
            }
            else if (entry.cfg_id == 3u)
            {
                prefix_len = (uint32_t)cli_tlv_entry_get_int(&entry);
            }
            else if (entry.cfg_id == 4u)
            {
                function_bits = (uint32_t)cli_tlv_entry_get_int(&entry);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    char error[256] = "";
    int rc;
    if (is_no)
    {
        rc = srv6_worker_locator_delete(name, error, sizeof(error));
    }
    else
    {
        srv6_locator_t locator;
        memset(&locator, 0, sizeof(locator));
        g_strlcpy(locator.name, name, sizeof(locator.name));
        locator.prefix_len = (uint8_t)prefix_len;
        locator.function_bits = (uint8_t)function_bits;
        if (prefix_len > 128u || function_bits == 0u || function_bits > SRV6_FUNCTION_BITS_MAX ||
            net_addr_from_str(prefix_text, &locator.prefix) != 0 || locator.prefix.family != AF_INET6)
        {
            g_strlcpy(error, "invalid locator prefix", sizeof(error));
            rc = ERRCODE_FAIL;
        }
        else
        {
            rc = srv6_worker_locator_upsert(&locator, error, sizeof(error));
        }
    }
    if (rc != ERRCODE_SUCCESS)
    {
        char response[320];
        snprintf(response, sizeof(response), "SRV6 Error: %s\r\n", error[0] ? error : "locator operation failed");
        srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP, response);
        return rc;
    }
    srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP, "");
    return ERRCODE_SUCCESS;
}

int srv6_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    cli_tlv_parser_t parser;
    if (!msg || cli_tlv_init(&parser, msg->payload, msg->payload_len) != 0)
    {
        if (msg)
        {
            srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP, "SRV6 Error: malformed command.\r\n");
        }
        return ERRCODE_FAIL;
    }
    int rc;
    switch (parser.group_id)
    {
        case SRV6_CLI_GROUP_PROTOCOL:
            rc = srv6_cli_handle_protocol(msg, &parser);
            break;
        case SRV6_CLI_GROUP_LOCATOR:
            rc = srv6_cli_handle_locator(msg, &parser);
            break;
        default:
            srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP, "SRV6 Error: unsupported command.\r\n");
            rc = ERRCODE_FAIL;
            break;
    }
    cli_tlv_cleanup(&parser);
    return rc;
}
