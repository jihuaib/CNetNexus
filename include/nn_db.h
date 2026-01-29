#ifndef NN_DB_H
#define NN_DB_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>

#include "nn_cfg.h"

typedef struct nn_db_field nn_db_field_t;
typedef struct nn_db_table nn_db_table_t;
typedef struct nn_db_definition nn_db_definition_t;
typedef struct nn_db_value nn_db_value_t;

/**
 * @brief Create a field definition
 * @param field_name Field name
 * @param type_str Type string (e.g., "uint(1-4294967295)")
 * @return Newly allocated field definition
 */
nn_db_field_t *nn_db_field_create(const char *field_name, const char *type_str);

/**
 * @brief Create a database definition
 * @param db_name Database name
 * @param module_id Module ID
 * @return Newly allocated database definition
 */
nn_db_definition_t *nn_db_definition_create(const char *db_name, uint32_t module_id);

/**
 * @brief Create a table definition
 * @param table_name Table name
 * @return Newly allocated table definition
 */
nn_db_table_t *nn_db_table_create(const char *table_name);

/**
 * @brief Add a field to a table
 * @param table Table to add field to
 * @param field Field to add (ownership transferred)
 */
void nn_db_table_add_field(nn_db_table_t *table, nn_db_field_t *field);

/**
 * @brief Add a table to a database definition
 * @param db_def Database definition
 * @param table Table to add (ownership transferred)
 */
void nn_db_definition_add_table(nn_db_definition_t *db_def, nn_db_table_t *table);

/**
 * @brief Add a database definition to the registry
 * @param db_def Database definition to add (ownership transferred)
 */
void nn_db_registry_add(nn_db_definition_t *db_def);

// ============================================================================
// Row/Result Types
// ============================================================================

typedef struct nn_db_row nn_db_row_t;
typedef struct nn_db_result nn_db_result_t;

// ============================================================================
// Initialization API (called by cfg module)
// ============================================================================

/**
 * @brief Initialize all databases from registered definitions
 * Creates database files and tables based on XML definitions
 * Called by cfg module after all XML files are loaded
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_initialize_all(void);

// ============================================================================
// CRUD Operations
// ============================================================================

/**
 * @brief Insert a row into a table
 * @param db_name Database name (e.g., "bgp_db")
 * @param table_name Table name (e.g., "bgp_protocol")
 * @param field_names Array of field names
 * @param values Array of values (must match field_names length)
 * @param num_fields Number of fields
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_insert(const char *db_name, const char *table_name, const char **field_names, const nn_db_value_t *values,
                 uint32_t num_fields);

/**
 * @brief Update rows matching a condition
 * @param db_name Database name
 * @param table_name Table name
 * @param field_names Array of fields to update
 * @param values Array of new values
 * @param num_fields Number of fields to update
 * @param where_clause SQL WHERE clause (e.g., "as_number = 65001") or NULL for all rows
 * @return Number of rows updated, or -1 on error
 */
int nn_db_update(const char *db_name, const char *table_name, const char **field_names, const nn_db_value_t *values,
                 uint32_t num_fields, const char *where_clause);

/**
 * @brief Delete rows matching a condition
 * @param db_name Database name
 * @param table_name Table name
 * @param where_clause SQL WHERE clause or NULL to delete all rows
 * @return Number of rows deleted, or -1 on error
 */
int nn_db_delete(const char *db_name, const char *table_name, const char *where_clause);

/**
 * @brief Query rows from a table
 * @param db_name Database name
 * @param table_name Table name
 * @param field_names Array of field names to retrieve (NULL for all fields: "*")
 * @param num_fields Number of fields (0 for all fields)
 * @param where_clause SQL WHERE clause or NULL for all rows
 * @param result Output result set (caller must free with nn_db_result_free)
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_query(const char *db_name, const char *table_name, const char **field_names, uint32_t num_fields,
                const char *where_clause, nn_db_result_t **result);

/**
 * @brief Check if a row exists matching a condition
 * @param db_name Database name
 * @param table_name Table name
 * @param where_clause SQL WHERE clause
 * @param exists Output boolean (TRUE if row exists)
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_exists(const char *db_name, const char *table_name, const char *where_clause, gboolean *exists);

// ============================================================================
// Memory Management
// ============================================================================

/**
 * @brief Free a query result
 * @param result Result to free
 */
void nn_db_result_free(nn_db_result_t *result);

/**
 * @brief Create a value from integer
 */
nn_db_value_t nn_db_value_int(int64_t value);

/**
 * @brief Create a value from string (makes copy)
 */
nn_db_value_t nn_db_value_text(const char *value);

/**
 * @brief Create a value from double
 */
nn_db_value_t nn_db_value_real(double value);

/**
 * @brief Create NULL value
 */
nn_db_value_t nn_db_value_null(void);

/**
 * @brief Free a value (frees allocated text)
 */
void nn_db_value_free(nn_db_value_t *value);

// ============================================================================
// Type Validation (leverages XML type definitions)
// ============================================================================

/**
 * @brief Validate a value against field's type definition from XML
 * @param db_name Database name
 * @param table_name Table name
 * @param field_name Field name
 * @param value Value to validate
 * @param error_msg Output error message buffer (optional)
 * @param error_msg_len Error message buffer length
 * @return TRUE if valid, FALSE if invalid
 */
gboolean nn_db_validate_field(const char *db_name, const char *table_name, const char *field_name,
                          const nn_db_value_t *value, char *error_msg, uint32_t error_msg_len);

#endif // NN_DB_H
