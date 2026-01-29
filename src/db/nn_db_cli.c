#include "nn_db_cli.h"

#include <stdio.h>
#include <string.h>

#include "nn_cfg.h"
#include "nn_db_registry.h"
#include "nn_dev.h"
#include "nn_errcode.h"

// ============================================================================
// Group Dispatch Table
// ============================================================================
typedef int (*nn_db_group_handler_t)(nn_cfg_tlv_parser_t parser, nn_db_cli_out_t *cfg_out,
                                     nn_db_cli_resp_out_t *resp_out);

typedef struct nn_db_group_dispatch
{
    uint32_t group_id;
    nn_db_group_handler_t handler;
} nn_db_group_dispatch_t;

static int handle_show_db(nn_cfg_tlv_parser_t parser, nn_db_cli_out_t *cfg_out, nn_db_cli_resp_out_t *resp_out);

static const nn_db_group_dispatch_t g_db_group_dispatch[] = {
    {NN_DB_CLI_GROUP_ID_SHOW_DB, handle_show_db},
};

#define DB_GROUP_DISPATCH_COUNT (sizeof(g_db_group_dispatch) / sizeof(g_db_group_dispatch[0]))

typedef int (*nn_db_cfg_resp_t)(nn_dev_message_t *msg, const nn_db_cli_out_t *cfg_out,
                                const nn_db_cli_resp_out_t *resp_out);

typedef struct nn_db_cli_resp_dispatch
{
    uint32_t group_id;
    nn_db_cfg_resp_t handler;
} nn_db_cli_resp_dispatch_t;

static int handle_default_resp(nn_dev_message_t *msg, const nn_db_cli_out_t *cfg_out,
                               const nn_db_cli_resp_out_t *resp_out);

static const nn_db_cli_resp_dispatch_t g_nn_db_cfg_resp_dispatch[] = {
    {NN_DB_CLI_GROUP_ID_SHOW_DB, handle_default_resp},
};

#define NN_DB_CFG_RESP_DISPATCH_COUNT (sizeof(g_nn_db_cfg_resp_dispatch) / sizeof(g_nn_db_cfg_resp_dispatch[0]))

// ============================================================================
// Show DB Command Handler
// ============================================================================

/**
 * @brief Handle "show db" command
 * Displays all registered databases, or tables in a db, or structure of a table
 */
static int handle_show_db(nn_cfg_tlv_parser_t parser, nn_db_cli_out_t *cfg_out, nn_db_cli_resp_out_t *resp_out)
{
    NN_CFG_TLV_FOREACH(parser, cfg_id, value, len)
    {
        switch (cfg_id)
        {
            case NN_DB_CLI_SHOW_DB_CFG_ID_LIST:
            {
                cfg_out->data.show_db.is_db_list = TRUE;
                break;
            }
            case NN_DB_CLI_SHOW_DB_CFG_ID_DB_NAME:
            {
                NN_CFG_TLV_GET_STRING(value, len, cfg_out->data.show_db.db_name, sizeof(cfg_out->data.show_db.db_name));
                break;
            }
            case NN_DB_CLI_SHOW_DB_CFG_ID_TABLE_LIST:
            {
                cfg_out->data.show_db.is_table_list = TRUE;
                break;
            }
            case NN_DB_CLI_SHOW_DB_CFG_ID_TABLE_FIELD:
            {
                cfg_out->data.show_db.is_table_field = TRUE;
                break;
            }
            case NN_DB_CLI_SHOW_DB_CFG_ID_TABLE_DATA:
            {
                cfg_out->data.show_db.is_table_data = TRUE;
                break;
            }
            case NN_DB_CLI_SHOW_DB_CFG_ID_TABLE_NAME:
            {
                NN_CFG_TLV_GET_STRING(value, len, cfg_out->data.show_db.table_name,
                                      sizeof(cfg_out->data.show_db.table_name));
                break;
            }
        }
    }

    nn_db_registry_t *registry = nn_db_registry_get_instance();
    if (!registry || !registry->databases)
    {
        snprintf(resp_out->message, sizeof(resp_out->message), "No databases registered\r\n");
        resp_out->success = 1;
        return NN_ERRCODE_SUCCESS;
    }

    int offset = 0;

    if (cfg_out->data.show_db.is_table_field)
    {
        // show db <db-name> table <table-name>
        nn_db_table_t *table =
            nn_db_registry_find_table(cfg_out->data.show_db.db_name, cfg_out->data.show_db.table_name);
        if (table)
        {
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "Database: %s, Table: %s\r\n", cfg_out->data.show_db.db_name, table->table_name);
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "Fields:\r\n");
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "  %-20s | %-20s | %-10s\r\n", "Field Name", "Type", "SQL Type");
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "  ------------------------------------------------------------\r\n");
            for (uint32_t j = 0; j < table->num_fields; j++)
            {
                nn_db_field_t *field = table->fields[j];
                offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                                   "  %-20s | %-20s | %-10s\r\n", field->field_name, field->type_str, field->sql_type);
            }
        }
        else
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Table '%s' not found in database '%s'\r\n",
                     cfg_out->data.show_db.table_name, cfg_out->data.show_db.db_name);
        }
    }
    else if (cfg_out->data.show_db.is_table_list)
    {
        // show db <db-name> table
        nn_db_definition_t *db_def = nn_db_registry_find(cfg_out->data.show_db.db_name);
        if (db_def)
        {
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "Database: %s\r\n",
                               db_def->db_name);
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "Tables:\r\n");
            for (uint32_t i = 0; i < db_def->num_tables; i++)
            {
                offset +=
                    snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "  - %s (%u fields)\r\n",
                             db_def->tables[i]->table_name, db_def->tables[i]->num_fields);
            }
        }
        else
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Database '%s' not found\r\n",
                     cfg_out->data.show_db.db_name);
        }
    }
    else if (cfg_out->data.show_db.is_db_list)
    {
        // show db
        offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "Registered Databases:\r\n");
        offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "=====================\r\n");
        offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "%-20s | %-12s | %-8s\r\n",
                           "Name", "Module", "Tables");
        offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                           "--------------------------------------------\r\n");

        g_mutex_lock(&registry->registry_mutex);
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, registry->databases);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            nn_db_definition_t *db_def = (nn_db_definition_t *)value;
            char module_name[32];
            if (nn_dev_get_module_name(db_def->module_id, module_name) != NN_ERRCODE_SUCCESS)
            {
                snprintf(module_name, sizeof(module_name), "0x%08X", db_def->module_id);
            }
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "%-20s | %-12s | %-8u\r\n", db_def->db_name, module_name, db_def->num_tables);
        }
        g_mutex_unlock(&registry->registry_mutex);
    }

    resp_out->success = 1;
    return NN_ERRCODE_SUCCESS;
}

// ============================================================================
// Dispatch logic
// ============================================================================

static int dispatch_by_group_id(uint32_t group_id, nn_cfg_tlv_parser_t parser, nn_db_cli_out_t *cfg_out,
                                nn_db_cli_resp_out_t *resp_out)
{
    for (size_t i = 0; i < DB_GROUP_DISPATCH_COUNT; i++)
    {
        if (g_db_group_dispatch[i].group_id == group_id)
        {
            printf("[db_cli] Dispatching to group (group_id=%u)\n", group_id);
            return g_db_group_dispatch[i].handler(parser, cfg_out, resp_out);
        }
    }

    printf("[db_cli] Error: Unknown group_id: %u\n", group_id);
    snprintf(resp_out->message, sizeof(resp_out->message), "DB Error: Unknown command group %u.\r\n", group_id);
    resp_out->success = 0;
    return NN_ERRCODE_FAIL;
}

static int handle_default_resp(nn_dev_message_t *msg, const nn_db_cli_out_t *cfg_out,
                               const nn_db_cli_resp_out_t *resp_out)
{
    (void)cfg_out;

    char *resp_data = g_strdup(resp_out->message);
    nn_dev_message_t *resp_msg = nn_dev_message_create(NN_CFG_MSG_TYPE_CLI_RESP, NN_DEV_MODULE_ID_DB, msg->request_id,
                                                       resp_data, strlen(resp_data) + 1, g_free);

    if (resp_msg)
    {
        nn_dev_pubsub_send_response(msg->sender_id, resp_msg);
        nn_dev_message_free(resp_msg);
    }

    return NN_ERRCODE_SUCCESS;
}

static void nn_db_cli_send_response(nn_dev_message_t *msg, const nn_db_cli_out_t *cfg_out,
                                    const nn_db_cli_resp_out_t *resp_out)
{
    if (msg->sender_id == 0)
    {
        return; // No sender to respond to
    }

    for (size_t i = 0; i < NN_DB_CFG_RESP_DISPATCH_COUNT; i++)
    {
        if (g_nn_db_cfg_resp_dispatch[i].group_id == cfg_out->group_id)
        {
            printf("[db_cli] Dispatching resp to group (group_id=%u)\n", cfg_out->group_id);
            (void)g_nn_db_cfg_resp_dispatch[i].handler(msg, cfg_out, resp_out);
        }
    }
}

int nn_db_cli_process_command(nn_dev_message_t *msg)
{
    if (!msg || !msg->data)
    {
        return NN_ERRCODE_FAIL;
    }

    nn_db_cli_out_t cfg_out;
    nn_db_cli_resp_out_t resp_out;
    memset(&cfg_out, 0, sizeof(cfg_out));
    memset(&resp_out, 0, sizeof(resp_out));

    int result = NN_ERRCODE_FAIL;

    NN_CFG_TLV_PARSE_BEGIN(msg->data, msg->data_len, parser, group_id)
    {
        printf("[db_cli] Received CLI command (group_id=%u)\n", group_id);
        cfg_out.group_id = group_id;
        result = dispatch_by_group_id(group_id, parser, &cfg_out, &resp_out);
    }
    NN_CFG_TLV_PARSE_END();

    // Send response back
    nn_db_cli_send_response(msg, &cfg_out, &resp_out);

    return result;
}
