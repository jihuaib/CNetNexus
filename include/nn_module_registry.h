#ifndef NN_MODULE_REGISTRY_H
#define NN_MODULE_REGISTRY_H

#include <glib.h>

// Forward declaration
typedef struct nn_module_mq nn_module_mq_t;

// Module initialization callback type
// Returns 0 on success, non-zero on failure
typedef int32_t (*nn_module_init_fn)(void);

// Module cleanup callback type
typedef void (*nn_module_cleanup_fn)(void);

// Module descriptor structure
typedef struct nn_module
{
    const char *name;              // Module name
    const char *xml_path;          // XML configuration file path (optional, can be NULL)
    nn_module_init_fn init;        // Module initialization function
    nn_module_cleanup_fn cleanup;  // Module cleanup function
    struct nn_module *next;        // Linked list pointer (for backward compatibility)
    
    // Message queue for inter-module communication
    nn_module_mq_t *mq;            // Module message queue (NULL if not initialized)
} nn_module_t;

// Register a module with init/cleanup callbacks
void nn_register_module(const char *name, const char *xml_path, nn_module_init_fn init, nn_module_cleanup_fn cleanup);

// Get module by name (O(1) hash table lookup)
nn_module_t *nn_get_module(const char *name);

// Get the head of the module registry (for backward compatibility)
nn_module_t *nn_get_modules(void);

// Get total number of registered modules
guint nn_get_module_count(void);

// Get the module registry hash table for iteration
// Returns GHashTable* (name -> nn_module_t*)
// Use GHashTableIter to iterate: g_hash_table_iter_init(&iter, nn_get_module_registry())
GHashTable *nn_get_module_registry(void);

// Initialize all registered modules (calls each module's init function)
int32_t nn_init_all_modules(void);

// Cleanup all registered modules (calls each module's cleanup function)
void nn_cleanup_all_modules(void);

// Request shutdown of all modules
void nn_request_shutdown(void);

// Check if shutdown was requested
int nn_shutdown_requested(void);

// Include message queue API
#include "nn_message_queue.h"

// Helper: Set module message queue
void nn_module_set_mq(nn_module_t *module, nn_module_mq_t *mq);

// Helper: Get module message queue
nn_module_mq_t *nn_module_get_mq(nn_module_t *module);

#endif // NN_MODULE_REGISTRY_H
