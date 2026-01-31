/**
 * @file   nn_db_registry.h
 * @brief  数据库定义注册表头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef NN_DB_REGISTRY_H
#define NN_DB_REGISTRY_H

#include <glib.h>
#include <stdint.h>

#include "nn_cfg.h"
#include "nn_db.h"

// Database field definition (parsed from XML <field> element)
struct nn_db_field
{
    char *field_name;                // Field name (e.g., "as_number")
    char *type_str;                  // Type string from XML (e.g., "uint(1-4294967295)")
    nn_cli_param_type_t *param_type; // Parsed parameter type (for validation)
    char *sql_type;                  // SQLite type ("INTEGER", "TEXT", "REAL")
};

// Table definition (parsed from XML <table> element)
struct nn_db_table
{
    char *table_name;         // Table name (e.g., "bgp_protocol")
    nn_db_field_t **fields;   // Array of field definitions
    uint32_t num_fields;      // Number of fields
    uint32_t fields_capacity; // Allocated capacity
};

// Database definition (parsed from XML <db> element)
struct nn_db_definition
{
    char *db_name;            // Database name (e.g., "bgp_db")
    uint32_t module_id;       // Module ID that owns this database
    nn_db_table_t **tables;   // Array of table definitions
    uint32_t num_tables;      // Number of tables
    uint32_t tables_capacity; // Allocated capacity
};

// ============================================================================
// Database Definition Structures (parsed from XML)
// ============================================================================

// Global registry of all database definitions
typedef struct nn_db_registry
{
    GHashTable *databases; // Map: db_name (char*) -> nn_db_definition_t*
    GMutex registry_mutex; // Thread-safe access
} nn_db_registry_t;

// ============================================================================
// Field Management Functions
// ============================================================================

/**
 * @brief Free a field definition
 * @param field Field to free
 */
void nn_db_field_free(nn_db_field_t *field);

// ============================================================================
// Table Management Functions
// ============================================================================

/**
 * @brief Free a table definition
 * @param table Table to free
 */
void nn_db_table_free(nn_db_table_t *table);

// ============================================================================
// Database Definition Management Functions
// ============================================================================

/**
 * @brief Free a database definition
 * @param db_def Database definition to free
 */
void nn_db_definition_free(nn_db_definition_t *db_def);

// ============================================================================
// Registry Management Functions
// ============================================================================

/**
 * @brief Create the global registry
 * @return Newly allocated registry
 */
nn_db_registry_t *nn_db_registry_create(void);

/**
 * @brief Find a database definition by name
 * @param db_name Database name
 * @return Database definition or NULL if not found
 */
nn_db_definition_t *nn_db_registry_find(const char *db_name);

/**
 * @brief Find a table definition within a database
 * @param db_name Database name
 * @param table_name Table name
 * @return Table definition or NULL if not found
 */
nn_db_table_t *nn_db_registry_find_table(const char *db_name, const char *table_name);

/**
 * @brief Find a field definition within a table
 * @param db_name Database name
 * @param table_name Table name
 * @param field_name Field name
 * @return Field definition or NULL if not found
 */
nn_db_field_t *nn_db_registry_find_field(const char *db_name, const char *table_name, const char *field_name);

/**
 * @brief Free the global registry
 */
void nn_db_registry_destroy(void);

/**
 * @brief Get the global registry instance
 * @return Global registry (creates if doesn't exist)
 */
nn_db_registry_t *nn_db_registry_get_instance(void);

#endif // NN_DB_REGISTRY_H
