#ifndef NN_DB_MAIN_H
#define NN_DB_MAIN_H

#include <glib.h>
#include <sqlite3.h>
#include <stdint.h>

#include "nn_db_registry.h"

// ============================================================================
// Runtime Database Connection
// ============================================================================

// Runtime database connection
typedef struct nn_db_connection
{
    char *db_path;   // Path to SQLite database file
    sqlite3 *handle; // SQLite handle
    GMutex db_mutex; // Per-database mutex for thread safety
} nn_db_connection_t;

// ============================================================================
// Module Context
// ============================================================================

// Module context (global state)
typedef struct nn_db_context
{
    GHashTable *connections;    // Map: db_name (char*) -> nn_db_connection_t*
    nn_db_registry_t *registry; // Database definitions registry

    // Event handling
    int epoll_fd; // Epoll instance for event handling
    int event_fd; // Eventfd for message queue notifications
    void *mq;     // Message queue (nn_dev_module_mq_t)
} nn_db_context_t;

// Global context instance
extern nn_db_context_t *g_nn_db_context;

// ============================================================================
// Internal Module Functions
// ============================================================================

/**
 * @brief Module initialization callback
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int32_t db_module_init(void *module);

/**
 * @brief Module cleanup callback
 */
void db_module_cleanup(void);

/**
 * @brief Get database connection by name
 * @param db_name Database name
 * @return Connection or NULL if not found
 */
nn_db_connection_t *nn_db_get_connection(const char *db_name);

// ============================================================================
// Schema Management Functions (nn_db_schema.c)
// ============================================================================

/**
 * @brief Create a database file and open connection
 * @param db_name Database name
 * @param db_path Path to database file
 * @param handle Output SQLite handle
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_create_database_file(const char *db_name, const char *db_path, sqlite3 **handle);

/**
 * @brief Create a table from its definition
 * @param handle SQLite handle
 * @param table_name Table name
 * @param table_def Table definition
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_create_table(sqlite3 *handle, const char *table_name, nn_db_table_t *table_def);

/**
 * @brief Initialize database schema from definition
 * @param db_def Database definition
 * @return NN_ERRCODE_SUCCESS or NN_ERRCODE_FAIL
 */
int nn_db_initialize_database(nn_db_definition_t *db_def);

#endif // NN_DB_MAIN_H
