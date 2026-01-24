#ifndef NN_DEV_H
#define NN_DEV_H

#include <glib.h>



#define NN_MODULE_ID_CFG 0x0000001
#define NN_MODULE_ID_DEV 0x0000002
#define NN_MODULE_ID_BGP 0x0000003

// Forward declaration
typedef struct nn_module_mq nn_module_mq_t;

// Module initialization callback type
// Returns 0 on success, non-zero on failure
typedef int32_t (*nn_module_init_fn)(void);

// Module cleanup callback type
typedef void (*nn_module_cleanup_fn)(void);

// Register a module with init/cleanup callbacks
void nn_dev_register_module(const char *name, const char *xml_path, nn_module_init_fn init, nn_module_cleanup_fn cleanup);

// Get module by name (O(1) hash table lookup)
nn_dev_module_t *nn_get_module(const char *name);

// Get the head of the module registry (for backward compatibility)
nn_dev_module_t *nn_get_modules(void);

// Get total number of registered modules
guint nn_get_module_count(void);

// Get the module registry hash table for iteration
// Returns GHashTable* (name -> nn_dev_module_t*)
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

// Helper: Set module message queue
void nn_module_set_mq(nn_dev_module_t *module, nn_module_mq_t *mq);

// Helper: Get module message queue
nn_module_mq_t *nn_module_get_mq(nn_dev_module_t *module);

// Create a message
nn_dev_message_t *nn_message_create(const char *type, void *data, size_t data_len, void (*free_fn)(void *));

// Free a message
void nn_message_free(nn_dev_message_t *msg);

// Create module message queue
nn_module_mq_t *nn_mq_create(void);

// Destroy module message queue
void nn_mq_destroy(nn_module_mq_t *mq);

// Send message to module queue (thread-safe)
int nn_mq_send(nn_module_mq_t *mq, nn_dev_message_t *msg);

// Receive message from queue (non-blocking, thread-safe)
nn_dev_message_t *nn_mq_receive(nn_module_mq_t *mq);

// Wait for message on eventfd (blocking with timeout)
// Returns: >0 if message available, 0 if timeout, <0 on error
int nn_mq_wait(nn_module_mq_t *mq, int timeout_ms);

// Get eventfd for external polling
int nn_mq_get_eventfd(nn_module_mq_t *mq);


#endif // NN_DEV_H

