#include "nn_db_cli.h"

#include <stdio.h>
#include <string.h>

#include "nn_cfg.h"
#include "nn_db_registry.h"
#include "nn_dev.h"
#include "nn_errcode.h"

// ============================================================================
// Show DB Command Handler
// ============================================================================

/**
 * @brief Handle "show db" command
 * Displays all registered databases with their tables and fields
 */
static int handle_show_db(nn_dev_message_t *msg)
{
    nn_db_registry_t *registry = nn_db_registry_get_instance();
    if (!registry || !registry->databases)
    {
        const char *error_msg = "No databases registered\n";
        nn_dev_message_t *resp = nn_dev_message_create(NN_CFG_MSG_TYPE_CLI_RESP, NN_DEV_MODULE_ID_DB, msg->request_id,
                                                       (void *)error_msg, strlen(error_msg) + 1, NULL);
        nn_dev_pubsub_send_response(msg->sender_id, resp);
        return NN_ERRCODE_SUCCESS;
    }

    // Build response string
    char response[4096];
    int offset = 0;

    offset += snprintf(response + offset, sizeof(response) - offset, "Database Summary:\n");
    offset += snprintf(response + offset, sizeof(response) - offset, "================\n\n");

    g_mutex_lock(&registry->registry_mutex);

    GHashTableIter iter;
    gpointer key, value;
    int db_count = 0;
    int total_tables = 0;

    g_hash_table_iter_init(&iter, registry->databases);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        nn_db_definition_t *db_def = (nn_db_definition_t *)value;
        db_count++;
        total_tables += db_def->num_tables;

        // Get module name
        char module_name[32];
        if (nn_dev_get_module_name(db_def->module_id, module_name) != NN_ERRCODE_SUCCESS)
        {
            snprintf(module_name, sizeof(module_name), "module_0x%08X", db_def->module_id);
        }

        offset += snprintf(response + offset, sizeof(response) - offset, "Database: %s\n", db_def->db_name);
        offset += snprintf(response + offset, sizeof(response) - offset, "  Module:  %s (ID: 0x%08X)\n", module_name,
                           db_def->module_id);
        offset += snprintf(response + offset, sizeof(response) - offset, "  Tables:  %u\n", db_def->num_tables);

        // List tables
        for (uint32_t i = 0; i < db_def->num_tables; i++)
        {
            nn_db_table_t *table = db_def->tables[i];
            offset += snprintf(response + offset, sizeof(response) - offset, "    - %s (%u fields)\n",
                               table->table_name, table->num_fields);

            // List fields
            for (uint32_t j = 0; j < table->num_fields; j++)
            {
                nn_db_field_t *field = table->fields[j];
                offset += snprintf(response + offset, sizeof(response) - offset, "        * %s: %s (SQL: %s)\n",
                                   field->field_name, field->type_str, field->sql_type);
            }
        }

        offset += snprintf(response + offset, sizeof(response) - offset, "\n");

        // Check if buffer is getting full
        if (offset >= sizeof(response) - 200)
        {
            offset += snprintf(response + offset, sizeof(response) - offset, "... (output truncated)\n");
            break;
        }
    }

    g_mutex_unlock(&registry->registry_mutex);

    // Summary
    offset += snprintf(response + offset, sizeof(response) - offset, "Summary:\n");
    offset += snprintf(response + offset, sizeof(response) - offset, "--------\n");
    offset += snprintf(response + offset, sizeof(response) - offset, "Total Databases: %d\n", db_count);
    offset += snprintf(response + offset, sizeof(response) - offset, "Total Tables:    %d\n", total_tables);

    // Send response
    nn_dev_message_t *resp = nn_dev_message_create(NN_CFG_MSG_TYPE_CLI_RESP, NN_DEV_MODULE_ID_DB, msg->request_id,
                                                   g_strdup(response), strlen(response) + 1, g_free);

    nn_dev_pubsub_send_response(msg->sender_id, resp);

    return NN_ERRCODE_SUCCESS;
}

// ============================================================================
// Main Command Dispatcher
// ============================================================================

int nn_db_cli_process_command(nn_dev_message_t *msg)
{
    if (!msg || !msg->data)
    {
        fprintf(stderr, "[db_cli] Invalid message\n");
        return NN_ERRCODE_FAIL;
    }

    NN_CFG_TLV_PARSE_BEGIN(msg->data, msg->data_len, parser, group_id)
    {
        printf("[bgp_cfg] Received CLI command (group_id=%u)\n", group_id);
        switch (group_id)
        {
            case NN_DB_CLI_GROUP_ID_SHOW_DB:
                return handle_show_db(msg);

            default:
                fprintf(stderr, "[db_cli] Unknown group_id: %u\n", group_id);
                return NN_ERRCODE_FAIL;
        }
    }
    NN_CFG_TLV_PARSE_END();
}
