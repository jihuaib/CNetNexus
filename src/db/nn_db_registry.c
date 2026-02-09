/**
 * @file   nn_db_registry.c
 * @brief  数据库定义注册表实现
 * @author jhb
 * @date   2026/01/22
 */
#include "nn_db_registry.h"

#include <stdio.h>
#include <string.h>

#include "nn_errcode.h"

// Global registry instance
static nn_db_registry_t *g_nn_db_registry = NULL;

// ============================================================================
// Type Conversion (XML type to SQLite type)
// ============================================================================

static const char *get_sql_type(const char *xml_type)
{
    if (strncmp(xml_type, "uint(", 5) == 0)
    {
        return "INTEGER";
    }
    if (strncmp(xml_type, "int(", 4) == 0)
    {
        return "INTEGER";
    }
    if (strncmp(xml_type, "string(", 7) == 0)
    {
        return "TEXT";
    }
    if (strcmp(xml_type, "ipv4") == 0 || strcmp(xml_type, "ipv6") == 0 || strcmp(xml_type, "ip") == 0)
    {
        return "TEXT";
    }
    if (strcmp(xml_type, "mac") == 0)
    {
        return "TEXT";
    }
    if (strncmp(xml_type, "float(", 6) == 0)
    {
        return "REAL";
    }

    return "TEXT"; // Default
}

// ============================================================================
// Field Management Functions
// ============================================================================

nn_db_field_t *nn_db_field_create(const char *field_name, const char *type_str)
{
    if (!field_name || !type_str)
    {
        return NULL;
    }

    nn_db_field_t *field = g_malloc0(sizeof(nn_db_field_t));
    field->field_name = g_strdup(field_name);
    field->type_str = g_strdup(type_str);

    // Parse type for validation
    field->param_type = nn_cfg_param_type_parse(type_str);

    // Map to SQLite type
    field->sql_type = g_strdup(get_sql_type(type_str));

    return field;
}

void nn_db_field_free(nn_db_field_t *field)
{
    if (!field)
    {
        return;
    }

    g_free(field->field_name);
    g_free(field->type_str);
    g_free(field->sql_type);

    if (field->param_type)
    {
        nn_cfg_param_type_free(field->param_type);
    }

    g_free(field);
}

// ============================================================================
// Table Management Functions
// ============================================================================

nn_db_table_t *nn_db_table_create(const char *table_name)
{
    if (!table_name)
    {
        return NULL;
    }

    nn_db_table_t *table = g_malloc0(sizeof(nn_db_table_t));
    table->table_name = g_strdup(table_name);
    table->fields = NULL;
    table->num_fields = 0;
    table->fields_capacity = 0;

    return table;
}

void nn_db_table_add_field(nn_db_table_t *table, nn_db_field_t *field)
{
    if (!table || !field)
    {
        return;
    }

    // Resize array if needed
    if (table->num_fields >= table->fields_capacity)
    {
        table->fields_capacity = (table->fields_capacity == 0) ? 4 : table->fields_capacity * 2;
        table->fields = g_realloc(table->fields, table->fields_capacity * sizeof(nn_db_field_t *));
    }

    table->fields[table->num_fields++] = field;
}

void nn_db_table_free(nn_db_table_t *table)
{
    if (!table)
    {
        return;
    }

    g_free(table->table_name);

    for (uint32_t i = 0; i < table->num_fields; i++)
    {
        nn_db_field_free(table->fields[i]);
    }
    g_free(table->fields);

    g_free(table);
}

// ============================================================================
// Database Definition Management Functions
// ============================================================================

nn_db_definition_t *nn_db_definition_create(const char *db_name, uint32_t module_id)
{
    if (!db_name)
    {
        return NULL;
    }

    nn_db_definition_t *db_def = g_malloc0(sizeof(nn_db_definition_t));
    db_def->db_name = g_strdup(db_name);
    db_def->module_id = module_id;
    db_def->tables = NULL;
    db_def->num_tables = 0;
    db_def->tables_capacity = 0;

    return db_def;
}

void nn_db_definition_add_table(nn_db_definition_t *db_def, nn_db_table_t *table)
{
    if (!db_def || !table)
    {
        return;
    }

    // Resize array if needed
    if (db_def->num_tables >= db_def->tables_capacity)
    {
        db_def->tables_capacity = (db_def->tables_capacity == 0) ? 4 : db_def->tables_capacity * 2;
        db_def->tables = g_realloc(db_def->tables, db_def->tables_capacity * sizeof(nn_db_table_t *));
    }

    db_def->tables[db_def->num_tables++] = table;
}

void nn_db_definition_free(nn_db_definition_t *db_def)
{
    if (!db_def)
    {
        return;
    }

    g_free(db_def->db_name);

    for (uint32_t i = 0; i < db_def->num_tables; i++)
    {
        nn_db_table_free(db_def->tables[i]);
    }
    g_free(db_def->tables);

    g_free(db_def);
}

// ============================================================================
// Registry Management Functions
// ============================================================================

nn_db_registry_t *nn_db_registry_create(void)
{
    nn_db_registry_t *registry = g_malloc0(sizeof(nn_db_registry_t));
    registry->databases = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)nn_db_definition_free);
    g_mutex_init(&registry->registry_mutex);

    return registry;
}

void nn_db_registry_add(nn_db_definition_t *db_def)
{
    if (!db_def || !g_nn_db_registry)
    {
        return;
    }

    g_mutex_lock(&g_nn_db_registry->registry_mutex);
    g_hash_table_insert(g_nn_db_registry->databases, db_def->db_name, db_def);
    g_mutex_unlock(&g_nn_db_registry->registry_mutex);

    printf("[db] Registered database definition: %s (module_id: 0x%08X)\n", db_def->db_name, db_def->module_id);
}

nn_db_definition_t *nn_db_registry_find(const char *db_name)
{
    if (!db_name || !g_nn_db_registry)
    {
        return NULL;
    }

    g_mutex_lock(&g_nn_db_registry->registry_mutex);
    nn_db_definition_t *db_def = g_hash_table_lookup(g_nn_db_registry->databases, db_name);
    g_mutex_unlock(&g_nn_db_registry->registry_mutex);

    return db_def;
}

nn_db_table_t *nn_db_registry_find_table(const char *db_name, const char *table_name)
{
    nn_db_definition_t *db_def = nn_db_registry_find(db_name);
    if (!db_def || !table_name)
    {
        return NULL;
    }

    for (uint32_t i = 0; i < db_def->num_tables; i++)
    {
        if (strcmp(db_def->tables[i]->table_name, table_name) == 0)
        {
            return db_def->tables[i];
        }
    }

    return NULL;
}

nn_db_field_t *nn_db_registry_find_field(const char *db_name, const char *table_name, const char *field_name)
{
    nn_db_table_t *table = nn_db_registry_find_table(db_name, table_name);
    if (!table || !field_name)
    {
        return NULL;
    }

    for (uint32_t i = 0; i < table->num_fields; i++)
    {
        if (strcmp(table->fields[i]->field_name, field_name) == 0)
        {
            return table->fields[i];
        }
    }

    return NULL;
}

void nn_db_registry_destroy(void)
{
    if (!g_nn_db_registry)
    {
        return;
    }

    g_mutex_clear(&g_nn_db_registry->registry_mutex);
    g_hash_table_destroy(g_nn_db_registry->databases);
    g_free(g_nn_db_registry);
    g_nn_db_registry = NULL;
}

nn_db_registry_t *nn_db_registry_get_instance(void)
{
    if (!g_nn_db_registry)
    {
        g_nn_db_registry = nn_db_registry_create();
    }
    return g_nn_db_registry;
}
