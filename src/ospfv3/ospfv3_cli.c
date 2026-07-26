/**
 * @file   ospfv3_cli.c
 * @brief  OSPFv3 configuration command handlers
 */

#include "ospfv3_cli.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "ospfv3.h"
#include "ospfv3_db.h"
#include "ospfv3_main.h"
#include "ospfv3_worker.h"
#include "vrf.h"

#define OSPFV3_IF_LOOP_ID_MIN 1u
#define OSPFV3_IF_LOOP_ID_MAX 1024u

static void ospfv3_cli_send_resp_typed(dev_ipc_message_t *msg, uint32_t msg_type, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(msg_type, DEV_MODULE_ID_OSPFV3, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1u, g_free);
    if (resp)
    {
        dev_ipc_send_response(ospfv3_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static void ospfv3_cli_send_resp(dev_ipc_message_t *msg, const char *text)
{
    ospfv3_cli_send_resp_typed(msg, CLI_MSG_TYPE_RESP, text);
}

static int ospfv3_cli_dispatch_apply(dev_ipc_message_t *msg, ospfv3_apply_cmd_t *apply)
{
    if (!apply || ospfv3_worker_dispatch_apply(apply) != ERRCODE_SUCCESS)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Server unavailable\r\n");
        return ERRCODE_FAIL;
    }
    if (apply->rc != OSPFV3_APPLY_RC_OK && apply->rc != OSPFV3_APPLY_RC_NOOP)
    {
        char buf[300];
        g_snprintf(buf, sizeof(buf), "%s\r\n", apply->errmsg[0] != '\0' ? apply->errmsg : "OSPFV3 Error: Apply failed");
        ospfv3_cli_send_resp(msg, buf);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

static const char *ospfv3_cli_if_ctx_idx_to_name(uint32_t if_idx)
{
    switch (if_idx)
    {
        case 1:
            return "GE-1";
        case 2:
            return "GE-2";
        case 3:
            return "GE-3";
        case 4:
            return "GE-4";
        case 5:
            return "GE-5";
        case 6:
            return "GE-6";
        case 7:
            return "GE-7";
        case 8:
            return "GE-8";
        default:
            return NULL;
    }
}

static int ospfv3_cli_resolve_ifname(uint32_t if_idx, uint32_t loop_id, char *ifname, size_t ifname_len)
{
    if (!ifname || ifname_len == 0u)
    {
        return ERRCODE_FAIL;
    }

    if (loop_id >= OSPFV3_IF_LOOP_ID_MIN && loop_id <= OSPFV3_IF_LOOP_ID_MAX)
    {
        g_snprintf(ifname, ifname_len, "loop%u", loop_id);
        return ERRCODE_SUCCESS;
    }

    const char *name = ospfv3_cli_if_ctx_idx_to_name(if_idx);
    if (!name)
    {
        return ERRCODE_FAIL;
    }
    g_strlcpy(ifname, name, ifname_len);
    return ERRCODE_SUCCESS;
}

static void ospfv3_cli_if_cfg_defaults(ospfv3_if_cfg_t *cfg, const char *ifname)
{
    memset(cfg, 0, sizeof(*cfg));
    g_strlcpy(cfg->ifname, ifname, sizeof(cfg->ifname));
    cfg->network_type = OSPFV3_NETWORK_BROADCAST;
    cfg->priority = OSPFV3_DEFAULT_PRIORITY;
    cfg->cost = OSPFV3_DEFAULT_COST;
    cfg->hello_interval = OSPFV3_DEFAULT_HELLO_INTERVAL;
    cfg->dead_interval = OSPFV3_DEFAULT_DEAD_INTERVAL;
}

static void ospfv3_cli_rollback_implicit_area(uint32_t process_id, uint32_t area_id)
{
    if (ospfv3_db_del_area(process_id, area_id) != ERRCODE_SUCCESS)
    {
        return;
    }

    ospfv3_apply_cmd_t rollback = {
        .op = OSPFV3_APPLY_AREA_DEL,
        .u.area_del =
            {
                .process_id = process_id,
                .area_id = area_id,
            },
    };
    (void)ospfv3_worker_dispatch_apply(&rollback);
}

static int ospfv3_cli_handle_instance(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t process_id = 0u;
    uint32_t context_process_id = 0u;
    char requested_vrf[IF_VRF_NAME_MAX] = VRF_PUBLIC_VRF_NAME;
    gboolean vrf_specified = FALSE;
    int parse_rc = 0;

    cli_tlv_entry_t entry;
    while ((parse_rc = cli_tlv_next(parser, &entry)) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_OSPFV3_PROCESS)
            {
                context_process_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
        }
        else if (entry.cfg_id == 2u)
        {
            (void)cli_tlv_entry_get_u32(&entry, &process_id);
        }
        else if (entry.cfg_id == 3u)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                g_strlcpy(requested_vrf, text, sizeof(requested_vrf));
                vrf_specified = TRUE;
            }
        }
        cli_tlv_entry_free(&entry);
    }
    if (parse_rc < 0)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid command payload\r\n");
        return ERRCODE_FAIL;
    }
    if (process_id == 0u)
    {
        process_id = context_process_id;
    }
    if (process_id == 0u)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Missing process ID\r\n");
        return ERRCODE_FAIL;
    }

    ospfv3_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    if (is_no)
    {
        if (ospfv3_db_del_instance(process_id) != ERRCODE_SUCCESS)
        {
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to delete process configuration\r\n");
            return ERRCODE_FAIL;
        }

        apply.op = OSPFV3_APPLY_INSTANCE_DEL;
        apply.u.instance_del.process_id = process_id;
        if (ospfv3_cli_dispatch_apply(msg, &apply) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }

        gboolean has_more = FALSE;
        if (db_rpc_exists(ospfv3_local_ipc_ctx(), OSPFV3_TABLE_INSTANCE, NULL, &has_more) == ERRCODE_SUCCESS &&
            !has_more)
        {
            ospfv3_cli_send_resp_typed(msg, CLI_MSG_TYPE_RESP_EXITING,
                                       "OSPFV3: last process removed, process exiting.\r\n");
            kill(getpid(), SIGTERM);
            return ERRCODE_SUCCESS;
        }

        ospfv3_cli_send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    uint32_t persisted_router_id = 0u;
    uint32_t vrf_id = VRF_PUBLIC_VRF_ID;
    char vrf_name[IF_VRF_NAME_MAX] = VRF_PUBLIC_VRF_NAME;
    int exists = ospfv3_db_get_instance(process_id, &persisted_router_id, &vrf_id, vrf_name, sizeof(vrf_name)) ==
                 ERRCODE_SUCCESS;
    if (exists && vrf_specified && strcmp(vrf_name, requested_vrf) != 0)
    {
        char buf[180];
        g_snprintf(buf, sizeof(buf), "OSPFv3 Error: process %u is already bound to VRF %s\r\n", process_id, vrf_name);
        ospfv3_cli_send_resp(msg, buf);
        return ERRCODE_FAIL;
    }
    if (!exists)
    {
        g_strlcpy(vrf_name, requested_vrf, sizeof(vrf_name));
        if (ospfv3_db_resolve_vrf(vrf_name, &vrf_id) != ERRCODE_SUCCESS)
        {
            ospfv3_cli_send_resp(msg, "OSPFv3 Error: VRF does not exist\r\n");
            return ERRCODE_FAIL;
        }
    }

    apply.op = OSPFV3_APPLY_INSTANCE_SET;
    apply.u.instance_set.process_id = process_id;
    apply.u.instance_set.router_id = persisted_router_id;
    apply.u.instance_set.vrf_id = vrf_id;
    g_strlcpy(apply.u.instance_set.vrf_name, vrf_name, sizeof(apply.u.instance_set.vrf_name));
    if (ospfv3_cli_dispatch_apply(msg, &apply) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (ospfv3_db_set_instance(process_id, vrf_id, vrf_name) != ERRCODE_SUCCESS)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to persist process configuration\r\n");
        return ERRCODE_FAIL;
    }

    ospfv3_cli_send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int ospfv3_cli_handle_router_id(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t process_id = 0u;
    char router_id_text[INET_ADDRSTRLEN] = {0};
    int parse_rc = 0;

    cli_tlv_entry_t entry;
    while ((parse_rc = cli_tlv_next(parser, &entry)) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_OSPFV3_PROCESS)
            {
                process_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
        }
        else if (entry.cfg_id == 2u)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                g_strlcpy(router_id_text, text, sizeof(router_id_text));
            }
        }
        cli_tlv_entry_free(&entry);
    }
    if (parse_rc < 0)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid command payload\r\n");
        return ERRCODE_FAIL;
    }
    if (process_id == 0u)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: No OSPFV3 process context\r\n");
        return ERRCODE_FAIL;
    }

    uint32_t router_id = 0u;
    if (!is_no)
    {
        struct in_addr addr;
        if (router_id_text[0] == '\0' || inet_pton(AF_INET, router_id_text, &addr) != 1 ||
            addr.s_addr == htonl(INADDR_ANY))
        {
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid router ID\r\n");
            return ERRCODE_FAIL;
        }
        router_id = ntohl(addr.s_addr);
    }

    ospfv3_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = OSPFV3_APPLY_ROUTER_ID_SET;
    apply.u.router_id_set.process_id = process_id;
    apply.u.router_id_set.router_id = router_id;
    if (ospfv3_cli_dispatch_apply(msg, &apply) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (ospfv3_db_set_router_id(process_id, router_id) != ERRCODE_SUCCESS)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to persist router ID\r\n");
        return ERRCODE_FAIL;
    }

    ospfv3_cli_send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int ospfv3_cli_handle_area(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t process_id = 0u;
    uint32_t area_id = 0u;
    int has_area_id = 0;
    int parse_rc = 0;

    cli_tlv_entry_t entry;
    while ((parse_rc = cli_tlv_next(parser, &entry)) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_OSPFV3_PROCESS)
            {
                process_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
        }
        else if (entry.cfg_id == 2u)
        {
            has_area_id = cli_tlv_entry_get_u32(&entry, &area_id) == 0;
        }
        cli_tlv_entry_free(&entry);
    }
    if (parse_rc < 0 || !has_area_id)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid area command\r\n");
        return ERRCODE_FAIL;
    }
    if (process_id == 0u || ospfv3_db_get_instance(process_id, NULL, NULL, NULL, 0u) != ERRCODE_SUCCESS)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Process does not exist\r\n");
        return ERRCODE_FAIL;
    }

    ospfv3_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    if (is_no)
    {
        char ifname[IF_LOGICAL_NAME_MAX] = {0};
        int in_use = ospfv3_db_area_in_use(process_id, area_id, ifname, sizeof(ifname));
        if (in_use < 0)
        {
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to query area configuration\r\n");
            return ERRCODE_FAIL;
        }
        if (in_use > 0)
        {
            char response[300];
            g_snprintf(response, sizeof(response), "OSPFV3 Error: Area %u is in use by interface %s\r\n", area_id,
                       ifname[0] != '\0' ? ifname : "unknown");
            ospfv3_cli_send_resp(msg, response);
            return ERRCODE_FAIL;
        }

        apply.op = OSPFV3_APPLY_AREA_DEL;
        apply.u.area_del.process_id = process_id;
        apply.u.area_del.area_id = area_id;
        if (ospfv3_cli_dispatch_apply(msg, &apply) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
        if (ospfv3_db_del_area(process_id, area_id) != ERRCODE_SUCCESS)
        {
            ospfv3_apply_cmd_t rollback = {
                .op = OSPFV3_APPLY_AREA_SET,
                .u.area_set =
                    {
                        .process_id = process_id,
                        .area_id = area_id,
                    },
            };
            (void)ospfv3_worker_dispatch_apply(&rollback);
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to delete area configuration\r\n");
            return ERRCODE_FAIL;
        }
    }
    else
    {
        gboolean exists = FALSE;
        if (ospfv3_db_area_exists(process_id, area_id, &exists) != ERRCODE_SUCCESS)
        {
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to query area configuration\r\n");
            return ERRCODE_FAIL;
        }

        apply.op = OSPFV3_APPLY_AREA_SET;
        apply.u.area_set.process_id = process_id;
        apply.u.area_set.area_id = area_id;
        if (ospfv3_cli_dispatch_apply(msg, &apply) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
        if (!exists && ospfv3_db_set_area(process_id, area_id) != ERRCODE_SUCCESS)
        {
            ospfv3_apply_cmd_t rollback = {
                .op = OSPFV3_APPLY_AREA_DEL,
                .u.area_del =
                    {
                        .process_id = process_id,
                        .area_id = area_id,
                    },
            };
            (void)ospfv3_worker_dispatch_apply(&rollback);
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to persist area configuration\r\n");
            return ERRCODE_FAIL;
        }
    }

    ospfv3_cli_send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

typedef enum ospfv3_cli_if_action
{
    OSPFV3_CLI_IF_ACTION_NONE = 0,
    OSPFV3_CLI_IF_ACTION_ENABLE,
    OSPFV3_CLI_IF_ACTION_COST,
    OSPFV3_CLI_IF_ACTION_HELLO,
    OSPFV3_CLI_IF_ACTION_DEAD,
    OSPFV3_CLI_IF_ACTION_PRIORITY,
    OSPFV3_CLI_IF_ACTION_NETWORK_TYPE,
    OSPFV3_CLI_IF_ACTION_PASSIVE,
} ospfv3_cli_if_action_t;

static int ospfv3_cli_handle_interface(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t process_id = 0u;
    uint32_t if_idx = 0u;
    uint32_t loop_id = 0u;
    uint32_t area_id = 0u;
    uint32_t numeric_value = 0u;
    int has_area_keyword = 0;
    int has_area_value = 0;
    int has_numeric_value = 0;
    int has_broadcast = 0;
    int has_point_to_point = 0;
    int has_enable = 0;
    int has_cost = 0;
    int has_hello = 0;
    int has_dead = 0;
    int has_priority = 0;
    int has_network_type = 0;
    int has_passive = 0;
    int parse_rc = 0;

    cli_tlv_entry_t entry;
    while ((parse_rc = cli_tlv_next(parser, &entry)) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_IF_IDX)
            {
                if_idx = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            else if (entry.cfg_id == CLI_CTX_ID_IF_LOOP_IDX)
            {
                loop_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 2u:
                has_enable = 1;
                break;
            case 3u:
                has_cost = 1;
                break;
            case 4u:
                has_hello = 1;
                break;
            case 5u:
                has_dead = 1;
                break;
            case 6u:
                has_priority = 1;
                break;
            case 7u:
                has_network_type = 1;
                break;
            case 8u:
                has_passive = 1;
                break;
            case 9u:
                (void)cli_tlv_entry_get_u32(&entry, &process_id);
                break;
            case 10u:
                has_area_keyword = 1;
                break;
            case 11u:
                has_area_value = cli_tlv_entry_get_u32(&entry, &area_id) == 0;
                break;
            case 12u:
                has_numeric_value = cli_tlv_entry_get_u32(&entry, &numeric_value) == 0;
                break;
            case 13u:
                has_broadcast = 1;
                break;
            case 14u:
                has_point_to_point = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }
    if (parse_rc < 0)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid command payload\r\n");
        return ERRCODE_FAIL;
    }

    char ifname[IF_LOGICAL_NAME_MAX] = {0};
    if (ospfv3_cli_resolve_ifname(if_idx, loop_id, ifname, sizeof(ifname)) != ERRCODE_SUCCESS)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: No interface context\r\n");
        return ERRCODE_FAIL;
    }
    if (process_id == 0u)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Missing process ID\r\n");
        return ERRCODE_FAIL;
    }
    if (ospfv3_db_get_instance(process_id, NULL, NULL, NULL, 0u) != ERRCODE_SUCCESS)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Process does not exist\r\n");
        return ERRCODE_FAIL;
    }

    int action_count = has_enable + has_cost + has_hello + has_dead + has_priority + has_network_type + has_passive;
    if (action_count != 1)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid interface command\r\n");
        return ERRCODE_FAIL;
    }

    ospfv3_cli_if_action_t action = OSPFV3_CLI_IF_ACTION_NONE;
    if (has_enable)
    {
        action = OSPFV3_CLI_IF_ACTION_ENABLE;
    }
    else if (has_cost)
    {
        action = OSPFV3_CLI_IF_ACTION_COST;
    }
    else if (has_hello)
    {
        action = OSPFV3_CLI_IF_ACTION_HELLO;
    }
    else if (has_dead)
    {
        action = OSPFV3_CLI_IF_ACTION_DEAD;
    }
    else if (has_priority)
    {
        action = OSPFV3_CLI_IF_ACTION_PRIORITY;
    }
    else if (has_network_type)
    {
        action = OSPFV3_CLI_IF_ACTION_NETWORK_TYPE;
    }
    else if (has_passive)
    {
        action = OSPFV3_CLI_IF_ACTION_PASSIVE;
    }

    ospfv3_if_cfg_t cfg;
    gboolean implicit_area_created = FALSE;
    int has_cfg = ospfv3_db_get_interface(process_id, ifname, &cfg) == ERRCODE_SUCCESS;
    if (!has_cfg)
    {
        ospfv3_cli_if_cfg_defaults(&cfg, ifname);
    }

    if (action == OSPFV3_CLI_IF_ACTION_ENABLE)
    {
        if (is_no)
        {
            ospfv3_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.op = OSPFV3_APPLY_IF_DEL;
            apply.u.if_del.process_id = process_id;
            g_strlcpy(apply.u.if_del.ifname, ifname, sizeof(apply.u.if_del.ifname));
            if (ospfv3_cli_dispatch_apply(msg, &apply) != ERRCODE_SUCCESS)
            {
                return ERRCODE_FAIL;
            }
            if (ospfv3_db_del_interface(process_id, ifname) != ERRCODE_SUCCESS)
            {
                ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to delete interface configuration\r\n");
                return ERRCODE_FAIL;
            }
            ospfv3_cli_send_resp(msg, "");
            return ERRCODE_SUCCESS;
        }

        if (!has_area_keyword || !has_area_value)
        {
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Missing area ID\r\n");
            return ERRCODE_FAIL;
        }

        gboolean area_exists = FALSE;
        if (ospfv3_db_area_exists(process_id, area_id, &area_exists) != ERRCODE_SUCCESS)
        {
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to query area configuration\r\n");
            return ERRCODE_FAIL;
        }
        if (!area_exists)
        {
            ospfv3_apply_cmd_t area_apply = {
                .op = OSPFV3_APPLY_AREA_SET,
                .u.area_set =
                    {
                        .process_id = process_id,
                        .area_id = area_id,
                    },
            };
            if (ospfv3_cli_dispatch_apply(msg, &area_apply) != ERRCODE_SUCCESS)
            {
                return ERRCODE_FAIL;
            }
            if (ospfv3_db_set_area(process_id, area_id) != ERRCODE_SUCCESS)
            {
                ospfv3_cli_rollback_implicit_area(process_id, area_id);
                ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to persist area configuration\r\n");
                return ERRCODE_FAIL;
            }
            implicit_area_created = TRUE;
        }
        cfg.enabled = 1u;
        cfg.area_id = area_id;
    }
    else
    {
        if (!has_cfg || !cfg.enabled)
        {
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Interface is not enabled for this process\r\n");
            return ERRCODE_FAIL;
        }

        switch (action)
        {
            case OSPFV3_CLI_IF_ACTION_COST:
                if (!is_no && (!has_numeric_value || numeric_value == 0u || numeric_value > OSPFV3_MAX_COST))
                {
                    ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid interface cost\r\n");
                    return ERRCODE_FAIL;
                }
                cfg.cost = is_no ? OSPFV3_DEFAULT_COST : (uint16_t)numeric_value;
                break;
            case OSPFV3_CLI_IF_ACTION_HELLO:
                if (!is_no && (!has_numeric_value || numeric_value == 0u || numeric_value > UINT16_MAX))
                {
                    ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid hello interval\r\n");
                    return ERRCODE_FAIL;
                }
                cfg.hello_interval = is_no ? OSPFV3_DEFAULT_HELLO_INTERVAL : (uint16_t)numeric_value;
                break;
            case OSPFV3_CLI_IF_ACTION_DEAD:
                if (!is_no && (!has_numeric_value || numeric_value == 0u || numeric_value > UINT16_MAX))
                {
                    ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid dead interval\r\n");
                    return ERRCODE_FAIL;
                }
                cfg.dead_interval = is_no ? OSPFV3_DEFAULT_DEAD_INTERVAL : numeric_value;
                break;
            case OSPFV3_CLI_IF_ACTION_PRIORITY:
                if (!is_no && (!has_numeric_value || numeric_value > UINT8_MAX))
                {
                    ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid interface priority\r\n");
                    return ERRCODE_FAIL;
                }
                cfg.priority = is_no ? OSPFV3_DEFAULT_PRIORITY : (uint8_t)numeric_value;
                break;
            case OSPFV3_CLI_IF_ACTION_NETWORK_TYPE:
                if (!is_no && (has_broadcast + has_point_to_point != 1))
                {
                    ospfv3_cli_send_resp(msg, "OSPFV3 Error: Missing network type\r\n");
                    return ERRCODE_FAIL;
                }
                cfg.network_type = (is_no || has_broadcast) ? OSPFV3_NETWORK_BROADCAST : OSPFV3_NETWORK_POINT_TO_POINT;
                break;
            case OSPFV3_CLI_IF_ACTION_PASSIVE:
                cfg.passive = is_no ? 0u : 1u;
                break;
            default:
                ospfv3_cli_send_resp(msg, "OSPFV3 Error: Invalid interface command\r\n");
                return ERRCODE_FAIL;
        }
    }

    if (cfg.dead_interval <= cfg.hello_interval)
    {
        if (implicit_area_created)
        {
            ospfv3_cli_rollback_implicit_area(process_id, area_id);
        }
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Dead interval must be greater than hello interval\r\n");
        return ERRCODE_FAIL;
    }

    ospfv3_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = OSPFV3_APPLY_IF_SET;
    apply.u.if_set.process_id = process_id;
    apply.u.if_set.cfg = cfg;
    if (ospfv3_cli_dispatch_apply(msg, &apply) != ERRCODE_SUCCESS)
    {
        if (implicit_area_created)
        {
            ospfv3_cli_rollback_implicit_area(process_id, area_id);
        }
        return ERRCODE_FAIL;
    }
    if (ospfv3_db_set_interface(process_id, &cfg) != ERRCODE_SUCCESS)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to persist interface configuration\r\n");
        return ERRCODE_FAIL;
    }

    ospfv3_cli_send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

int ospfv3_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        ospfv3_cli_send_resp(msg, "OSPFV3 Error: Failed to parse command\r\n");
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    switch (parser.group_id)
    {
        case OSPFV3_CLI_GROUP_INSTANCE:
            rc = ospfv3_cli_handle_instance(msg, &parser);
            break;
        case OSPFV3_CLI_GROUP_ROUTER_ID:
            rc = ospfv3_cli_handle_router_id(msg, &parser);
            break;
        case OSPFV3_CLI_GROUP_INTERFACE:
            rc = ospfv3_cli_handle_interface(msg, &parser);
            break;
        case OSPFV3_CLI_GROUP_AREA:
            rc = ospfv3_cli_handle_area(msg, &parser);
            break;
        default:
            ospfv3_cli_send_resp(msg, "OSPFV3 Error: Unknown configuration command\r\n");
            rc = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return rc;
}
