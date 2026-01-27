#ifndef NN_DB_H
#define NN_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Field Value Types
// ============================================================================
typedef enum nn_db_value_type
{
    NN_DB_TYPE_NULL,
    NN_DB_TYPE_INTEGER,
    NN_DB_TYPE_REAL,
    NN_DB_TYPE_TEXT,
    NN_DB_TYPE_BLOB
} nn_db_value_type_t;

// Field value container
typedef struct nn_db_value
{
    nn_db_value_type_t type;
    union
    {
        int64_t i64; // INTEGER
        double real; // REAL
        char *text;  // TEXT (allocated, must be freed)
        struct
        {
            void *data; // BLOB data
            size_t len; // BLOB length
        } blob;
    } data;
} nn_db_value_t;

// ============================================================================
// Row/Result Types
// ============================================================================
typedef struct nn_db_row
{
    char **field_names;    // Array of field names
    nn_db_value_t *values; // Array of values
    uint32_t num_fields;   // Number of fields
} nn_db_row_t;

typedef struct nn_db_result
{
    nn_db_row_t **rows;     // Array of rows
    uint32_t num_rows;      // Number of rows
    uint32_t rows_capacity; // Allocated capacity
} nn_db_result_t;

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
 * @param exists Output boolean (true if row exists)
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_exists(const char *db_name, const char *table_name, const char *where_clause, bool *exists);

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
 * @return true if valid, false if invalid
 */
bool nn_db_validate_field(const char *db_name, const char *table_name, const char *field_name,
                          const nn_db_value_t *value, char *error_msg, uint32_t error_msg_len);

#endif // NN_DB_H
