#include "nn_if_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_cfg.h"
#include "nn_dev.h"
#include "nn_dev_pubsub.h"
#include "nn_errcode.h"
#include "nn_if.h"
#include "nn_if_map.h"

// Group Dispatch Table
typedef int (*nn_if_group_handler_t)(nn_cfg_tlv_parser_t parser, nn_if_cli_out_t *cfg_out,
                                     nn_if_cli_resp_out_t *resp_out);

static int handle_interface_cmd(nn_cfg_tlv_parser_t parser, nn_if_cli_out_t *cfg_out, nn_if_cli_resp_out_t *resp_out);
static int handle_config_cmd(nn_cfg_tlv_parser_t parser, nn_if_cli_out_t *cfg_out, nn_if_cli_resp_out_t *resp_out);
static int handle_show_cmd(nn_cfg_tlv_parser_t parser, nn_if_cli_out_t *cfg_out, nn_if_cli_resp_out_t *resp_out);

static const struct
{
    uint32_t group_id;
    nn_if_group_handler_t handler;
} g_if_group_dispatch[] = {
    {NN_IF_CLI_GROUP_ID_INTERFACE, handle_interface_cmd},
    {NN_IF_CLI_GROUP_ID_CONFIG, handle_config_cmd},
    {NN_IF_CLI_GROUP_ID_SHOW, handle_show_cmd},
};

#define IF_GROUP_DISPATCH_COUNT (sizeof(g_if_group_dispatch) / sizeof(g_if_group_dispatch[0]))

// Response Dispatch
typedef int (*nn_if_cfg_resp_t)(nn_dev_message_t *msg, const nn_if_cli_out_t *cfg_out,
                                const nn_if_cli_resp_out_t *resp_out);

static int handle_default_resp(nn_dev_message_t *msg, const nn_if_cli_out_t *cfg_out,
                               const nn_if_cli_resp_out_t *resp_out);

static const struct
{
    uint32_t group_id;
    nn_if_cfg_resp_t handler;
} g_if_cfg_resp_dispatch[] = {
    {NN_IF_CLI_GROUP_ID_INTERFACE, handle_default_resp},
    {NN_IF_CLI_GROUP_ID_CONFIG, handle_default_resp},
    {NN_IF_CLI_GROUP_ID_SHOW, handle_default_resp},
};

#define IF_CFG_RESP_DISPATCH_COUNT (sizeof(g_if_cfg_resp_dispatch) / sizeof(g_if_cfg_resp_dispatch[0]))

// Handlers implementation

static int handle_interface_cmd(nn_cfg_tlv_parser_t parser, nn_if_cli_out_t *cfg_out, nn_if_cli_resp_out_t *resp_out)
{
    NN_CFG_TLV_FOREACH(parser, cfg_id, value, len)
    {
        switch (cfg_id)
        {
            case NN_IF_CLI_IF_CFG_ID_GE1:
                strcpy(cfg_out->data.interface.ifname, "GE-1");
                break;
            case NN_IF_CLI_IF_CFG_ID_GE2:
                strcpy(cfg_out->data.interface.ifname, "GE-2");
                break;
            case NN_IF_CLI_IF_CFG_ID_GE3:
                strcpy(cfg_out->data.interface.ifname, "GE-3");
                break;
            case NN_IF_CLI_IF_CFG_ID_GE4:
                strcpy(cfg_out->data.interface.ifname, "GE-4");
                break;
        }
    }

    const char *phys_name = nn_if_map_get_physical(cfg_out->data.interface.ifname);
    if (!nn_if_exists(phys_name))
    {
        snprintf(resp_out->message, sizeof(resp_out->message), "Error: Interface %s does not exist\r\n",
                 cfg_out->data.interface.ifname);
        resp_out->success = 0;
        return NN_ERRCODE_FAIL;
    }

    resp_out->success = 1;
    return NN_ERRCODE_SUCCESS;
}

static int handle_config_cmd(nn_cfg_tlv_parser_t parser, nn_if_cli_out_t *cfg_out, nn_if_cli_resp_out_t *resp_out)
{
    NN_CFG_TLV_FOREACH(parser, cfg_id, value, len)
    {
        switch (cfg_id)
        {
            case NN_IF_CLI_IF_CFG_ID_IP:
                NN_CFG_TLV_GET_STRING(value, len, cfg_out->data.config.ip, sizeof(cfg_out->data.config.ip));
                cfg_out->data.config.has_ip = TRUE;
                break;
            case NN_IF_CLI_IF_CFG_ID_MASK:
                NN_CFG_TLV_GET_STRING(value, len, cfg_out->data.config.mask, sizeof(cfg_out->data.config.mask));
                break;
            case NN_IF_CLI_IF_CFG_ID_SHUTDOWN:
                cfg_out->data.config.shutdown = TRUE;
                break;
            case NN_IF_CLI_IF_CFG_ID_UNDO:
                cfg_out->data.config.undo = TRUE;
                break;
        }
    }

    // This handler would normally execute the change on the hardware/system
    // For now, we'll just report success and let the main loop or bg thread handle persistent state if needed
    // Actually, we should call the implementation in nn_if.c

    // We need to know WHICH interface we are configuring. The cfg_out doesn't have it because it's in the view context
    // in the session. In our architecture, the cfg module should have passed the session context or the "config mode"
    // should be handled. Currently, g_current_interface in nn_if.c is a global, which is NOT session-safe but matches
    // the current single-session design.

    // Wait, the way interface mode works is that the view-switching happens in cfg_cli.c (handle_default_resp).
    // But we need to keep track of WHICH interface we entered.

    extern char g_current_interface[IFNAMSIZ];
    if (g_current_interface[0] == '\0')
    {
        snprintf(resp_out->message, sizeof(resp_out->message), "Error: No interface selected\r\n");
        resp_out->success = 0;
        return NN_ERRCODE_FAIL;
    }

    if (cfg_out->data.config.has_ip)
    {
        if (nn_if_set_ip(g_current_interface, cfg_out->data.config.ip, cfg_out->data.config.mask) == NN_ERRCODE_SUCCESS)
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "IP address configured successfully on %s\r\n",
                     g_current_interface);
        }
        else
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Failed to set IP address on %s\r\n",
                     g_current_interface);
            resp_out->success = 0;
            return NN_ERRCODE_FAIL;
        }
    }
    else if (cfg_out->data.config.shutdown)
    {
        int state = cfg_out->data.config.undo ? 1 : 0;
        if (nn_if_set_state(g_current_interface, state) == NN_ERRCODE_SUCCESS)
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Interface %s %s\r\n", g_current_interface,
                     state ? "enabled" : "disabled");
        }
        else
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Failed to change state for %s\r\n",
                     g_current_interface);
            resp_out->success = 0;
            return NN_ERRCODE_FAIL;
        }
    }

    resp_out->success = 1;
    return NN_ERRCODE_SUCCESS;
}

static int handle_show_cmd(nn_cfg_tlv_parser_t parser, nn_if_cli_out_t *cfg_out, nn_if_cli_resp_out_t *resp_out)
{
    NN_CFG_TLV_FOREACH(parser, cfg_id, value, len)
    {
        if (cfg_id == NN_IF_CLI_IF_CFG_ID_SHOW_NAME)
        {
            NN_CFG_TLV_GET_STRING(value, len, cfg_out->data.show.ifname, sizeof(cfg_out->data.show.ifname));
            cfg_out->data.show.has_ifname = TRUE;
        }
    }

    int offset = 0;
    if (cfg_out->data.show.has_ifname)
    {
        nn_if_info_t info;
        if (nn_if_get_info(cfg_out->data.show.ifname, &info) == NN_ERRCODE_SUCCESS)
        {
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "Interface %s:\r\n",
                               info.name);
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "  Type: %s\r\n",
                               nn_if_type_to_string(info.type));
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "  State: %s\r\n",
                               info.state == NN_IF_STATE_UP ? "UP" : "DOWN");
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "  IP: %s\r\n",
                               info.ip_address[0] ? info.ip_address : "not configured");
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "  Netmask: %s\r\n",
                               info.netmask[0] ? info.netmask : "not configured");
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "  MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n", info.mac[0], info.mac[1], info.mac[2],
                               info.mac[3], info.mac[4], info.mac[5]);
            offset +=
                snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "  MTU: %d\r\n", info.mtu);
        }
        else
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Interface %s not found\r\n",
                     cfg_out->data.show.ifname);
            resp_out->success = 0;
            return NN_ERRCODE_FAIL;
        }
    }
    else
    {
        nn_if_info_t *interfaces = NULL;
        int count = 0;
        if (nn_if_list(&interfaces, &count) == NN_ERRCODE_SUCCESS)
        {
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "Interface Status:\r\n");
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "%-10s %-15s %-10s %-15s\r\n", "Name", "Type", "State", "IP Address");
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "%-10s %-15s %-10s %-15s\r\n", "----", "----", "-----", "----------");

            for (int i = 0; i < count; i++)
            {
                offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                                   "%-10s %-15s %-10s %-15s\r\n", interfaces[i].name,
                                   nn_if_type_to_string(interfaces[i].type),
                                   interfaces[i].state == NN_IF_STATE_UP ? "UP" : "DOWN",
                                   interfaces[i].ip_address[0] ? interfaces[i].ip_address : "-");
            }
            g_free(interfaces);
        }
        else
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Failed to list interfaces\r\n");
            resp_out->success = 0;
            return NN_ERRCODE_FAIL;
        }
    }

    resp_out->success = 1;
    return NN_ERRCODE_SUCCESS;
}

// Dispatch

static int dispatch_by_group_id(uint32_t group_id, nn_cfg_tlv_parser_t parser, nn_if_cli_out_t *cfg_out,
                                nn_if_cli_resp_out_t *resp_out)
{
    for (size_t i = 0; i < IF_GROUP_DISPATCH_COUNT; i++)
    {
        if (g_if_group_dispatch[i].group_id == group_id)
        {
            return g_if_group_dispatch[i].handler(parser, cfg_out, resp_out);
        }
    }
    return NN_ERRCODE_FAIL;
}

static int handle_default_resp(nn_dev_message_t *msg, const nn_if_cli_out_t *cfg_out,
                               const nn_if_cli_resp_out_t *resp_out)
{
    char *resp_data = NULL;
    size_t resp_len = 0;
    uint32_t msg_type = NN_CFG_MSG_TYPE_CLI_RESP;

    if (cfg_out->group_id == NN_IF_CLI_GROUP_ID_INTERFACE && resp_out->success)
    {
        msg_type = NN_CFG_MSG_TYPE_CLI_VIEW_CHG;
        char filled_prompt[NN_CFG_CLI_MAX_PROMPT_LEN];
        snprintf(filled_prompt, sizeof(filled_prompt), "<NetNexus(config-if-%s)>", cfg_out->data.interface.ifname);
        resp_data = g_strdup(filled_prompt);

        // Update global interface context
        extern char g_current_interface[IFNAMSIZ];
        strncpy(g_current_interface, cfg_out->data.interface.ifname, IFNAMSIZ - 1);
    }
    else
    {
        resp_data = g_strdup(resp_out->message);
    }

    if (resp_data)
    {
        resp_len = strlen(resp_data) + 1;
        nn_dev_message_t *resp_msg =
            nn_dev_message_create(msg_type, NN_DEV_MODULE_ID_IF, msg->request_id, resp_data, resp_len, g_free);

        if (resp_msg)
        {
            nn_dev_pubsub_send_response(msg->sender_id, resp_msg);
            nn_dev_message_free(resp_msg);
        }
    }

    return NN_ERRCODE_SUCCESS;
}

void nn_if_cli_send_response(nn_dev_message_t *msg, const nn_if_cli_out_t *cfg_out,
                             const nn_if_cli_resp_out_t *resp_out)
{
    if (msg->sender_id == 0)
    {
        return;
    }

    for (size_t i = 0; i < IF_CFG_RESP_DISPATCH_COUNT; i++)
    {
        if (g_if_cfg_resp_dispatch[i].group_id == cfg_out->group_id)
        {
            (void)g_if_cfg_resp_dispatch[i].handler(msg, cfg_out, resp_out);
            return;
        }
    }
}

int nn_if_cli_handle_message(nn_dev_message_t *msg)
{
    if (!msg || !msg->data)
    {
        return NN_ERRCODE_FAIL;
    }

    nn_if_cli_out_t cfg_out;
    nn_if_cli_resp_out_t resp_out;
    memset(&cfg_out, 0, sizeof(cfg_out));
    memset(&resp_out, 0, sizeof(resp_out));

    int result = NN_ERRCODE_FAIL;

    NN_CFG_TLV_PARSE_BEGIN(msg->data, msg->data_len, parser, group_id)
    {
        cfg_out.group_id = group_id;
        result = dispatch_by_group_id(group_id, parser, &cfg_out, &resp_out);
    }
    NN_CFG_TLV_PARSE_END();

    nn_if_cli_send_response(msg, &cfg_out, &resp_out);

    return result;
}
