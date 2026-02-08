/**
 * @file   db_registry.c
 * @brief  数据库定义注册表实现
 * @author jhb
 * @date   2026/01/22
 */
#include "db_registry.h"

#include <stdio.h>
#include <string.h>

#include "cli_param_type.h"
#include "errcode.h"

// Global registry instance
static db_registry_t *g_db_registry = NULL;

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

db_field_t *db_field_create(const char *field_name, const char *type_str)
{
    if (!field_name || !type_str)
    {
        return NULL;
    }

    db_field_t *field = g_malloc0(sizeof(db_field_t));
    field->field_name = g_strdup(field_name);
    field->type_str = g_strdup(type_str);

    // Parse type for validation
    field->param_type = cli_param_type_parse(type_str);

    // Map to SQLite type
    field->sql_type = g_strdup(get_sql_type(type_str));

    return field;
}

void db_field_free(db_field_t *field)
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
        cli_param_type_free(field->param_type);
    }

    g_free(field);
}

// ============================================================================
// Table Management Functions
// ============================================================================

db_table_t *db_table_create(const char *table_name)
{
    if (!table_name)
    {
        return NULL;
    }

    db_table_t *table = g_malloc0(sizeof(db_table_t));
    table->table_name = g_strdup(table_name);
    table->fields = NULL;
    table->num_fields = 0;
    table->fields_capacity = 0;

    return table;
}

void db_table_add_field(db_table_t *table, db_field_t *field)
{
    if (!table || !field)
    {
        return;
    }

    // Resize array if needed
    if (table->num_fields >= table->fields_capacity)
    {
        table->fields_capacity = (table->fields_capacity == 0) ? 4 : table->fields_capacity * 2;
        table->fields = g_realloc(table->fields, table->fields_capacity * sizeof(db_field_t *));
    }

    table->fields[table->num_fields++] = field;
}

void db_table_free(db_table_t *table)
{
    if (!table)
    {
        return;
    }

    g_free(table->table_name);

    for (uint32_t i = 0; i < table->num_fields; i++)
    {
        db_field_free(table->fields[i]);
    }
    g_free(table->fields);

    g_free(table);
}

// ============================================================================
// Database Definition Management Functions
// ============================================================================

db_definition_t *db_definition_create(const char *db_name, uint32_t module_id)
{
    if (!db_name)
    {
        return NULL;
    }

    db_definition_t *db_def = g_malloc0(sizeof(db_definition_t));
    db_def->db_name = g_strdup(db_name);
    db_def->module_id = module_id;
    db_def->tables = NULL;
    db_def->num_tables = 0;
    db_def->tables_capacity = 0;

    return db_def;
}

void db_definition_add_table(db_definition_t *db_def, db_table_t *table)
{
    if (!db_def || !table)
    {
        return;
    }

    // Resize array if needed
    if (db_def->num_tables >= db_def->tables_capacity)
    {
        db_def->tables_capacity = (db_def->tables_capacity == 0) ? 4 : db_def->tables_capacity * 2;
        db_def->tables = g_realloc(db_def->tables, db_def->tables_capacity * sizeof(db_table_t *));
    }

    db_def->tables[db_def->num_tables++] = table;
}

void db_definition_free(db_definition_t *db_def)
{
    if (!db_def)
    {
        return;
    }

    g_free(db_def->db_name);

    for (uint32_t i = 0; i < db_def->num_tables; i++)
    {
        db_table_free(db_def->tables[i]);
    }
    g_free(db_def->tables);

    g_free(db_def);
}

// ============================================================================
// Registry Management Functions
// ============================================================================

db_registry_t *db_registry_create(void)
{
    db_registry_t *registry = g_malloc0(sizeof(db_registry_t));
    registry->databases = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)db_definition_free);
    g_mutex_init(&registry->registry_mutex);

    return registry;
}

void db_registry_add(db_definition_t *db_def)
{
    if (!db_def || !g_db_registry)
    {
        return;
    }

    g_mutex_lock(&g_db_registry->registry_mutex);
    g_hash_table_insert(g_db_registry->databases, db_def->db_name, db_def);
    g_mutex_unlock(&g_db_registry->registry_mutex);

    printf("[db] Registered database definition: %s (module_id: 0x%08X)\n", db_def->db_name, db_def->module_id);
}

db_definition_t *db_registry_find(const char *db_name)
{
    if (!db_name || !g_db_registry)
    {
        return NULL;
    }

    g_mutex_lock(&g_db_registry->registry_mutex);
    db_definition_t *db_def = g_hash_table_lookup(g_db_registry->databases, db_name);
    g_mutex_unlock(&g_db_registry->registry_mutex);

    return db_def;
}

db_table_t *db_registry_find_table(const char *db_name, const char *table_name)
{
    db_definition_t *db_def = db_registry_find(db_name);
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

db_field_t *db_registry_find_field(const char *db_name, const char *table_name, const char *field_name)
{
    db_table_t *table = db_registry_find_table(db_name, table_name);
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

void db_registry_destroy(void)
{
    if (!g_db_registry)
    {
        return;
    }

    g_mutex_clear(&g_db_registry->registry_mutex);
    g_hash_table_destroy(g_db_registry->databases);
    g_free(g_db_registry);
    g_db_registry = NULL;
}

db_registry_t *db_registry_get_instance(void)
{
    if (!g_db_registry)
    {
        g_db_registry = db_registry_create();
    }
    return g_db_registry;
}
