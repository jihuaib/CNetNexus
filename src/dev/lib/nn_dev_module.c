#include "nn_dev.h"

#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global module registry (GLib hash table: name -> nn_dev_module_t*)
static GHashTable *g_module_registry = NULL;

// Global shutdown flag
static volatile sig_atomic_t g_shutdown_requested = 0;

// Initialize module registry
static void ensure_registry_initialized(void)
{
    if (!g_module_registry)
    {
        g_module_registry = g_hash_table_new(g_str_hash, g_str_equal);
    }
}

// Register a module
void nn_dev_register_module(uint32_t id, const char *name, nn_module_init_fn init, nn_module_cleanup_fn cleanup)
{
    if (!name)
    {
        return;
    }

    ensure_registry_initialized();

    nn_dev_module_t *module = (nn_dev_module_t *)g_malloc(sizeof(nn_dev_module_t));
    if (!module)
    {
        fprintf(stderr, "Failed to allocate memory for module: %s\n", name);
        return;
    }

    module->module_id = id;
    module->name = g_strdup(name);
    module->init = init;
    module->cleanup = cleanup;
    module->next = NULL; // Keep for backward compatibility
    module->mq = NULL;   // Message queue not initialized yet

    // Add to hash table for fast lookup
    g_hash_table_insert(g_module_registry, (gpointer)module->name, module);

    printf("Registered module: %s\n", name);
}

// Get module by name (O(1) lookup)
nn_dev_module_t *nn_get_module(const char *name)
{
    if (!g_module_registry || !name)
    {
        return NULL;
    }
    return (nn_dev_module_t *)g_hash_table_lookup(g_module_registry, name);
}

// Get the head of the module registry (for backward compatibility)
// Returns first module found (order not guaranteed)
nn_dev_module_t *nn_get_modules(void)
{
    if (!g_module_registry)
    {
        return NULL;
    }
    
    GHashTableIter iter;
    gpointer key, value;
    
    g_hash_table_iter_init(&iter, g_module_registry);
    if (g_hash_table_iter_next(&iter, &key, &value))
    {
        return (nn_dev_module_t *)value;
    }
    
    return NULL;
}

// Get module count
guint nn_get_module_count(void)
{
    return g_module_registry ? g_hash_table_size(g_module_registry) : 0;
}

// Get the module registry hash table for direct iteration
GHashTable *nn_get_module_registry(void)
{
    return g_module_registry;
}

// Initialize all registered modules
int32_t nn_init_all_modules(void)
{
    int32_t failed_count = 0;

    printf("\nInitializing modules:\n");
    printf("=====================\n");

    if (!g_module_registry)
    {
        printf("No modules registered\n");
        return 0;
    }

    // Iterate through all modules using GHashTable iterator
    GHashTableIter iter;
    gpointer key, value;
    
    g_hash_table_iter_init(&iter, g_module_registry);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        nn_dev_module_t *module = (nn_dev_module_t *)value;
        printf("Initializing module: %s\n", module->name);

        if (module->init)
        {
            if (module->init() == 0)
            {
                printf("  [OK] %s initialized\n", module->name);
            }
            else
            {
                fprintf(stderr, "  [FAIL] %s initialization failed\n", module->name);
                failed_count++;
            }
        }
        else
        {
            printf("  [SKIP] %s has no init function\n", module->name);
        }
    }

    printf("\nModule initialization complete (failures: %d)\n\n", failed_count);

    return failed_count;
}

// Cleanup all registered modules
void nn_cleanup_all_modules(void)
{
    printf("\nCleaning up modules:\n");
    printf("====================\n");

    if (!g_module_registry)
    {
        printf("No modules to clean up\n");
        return;
    }

    // Cleanup all modules using iterator
    GHashTableIter iter;
    gpointer key, value;
    
    g_hash_table_iter_init(&iter, g_module_registry);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        nn_dev_module_t *module = (nn_dev_module_t *)value;
        printf("Cleaning up module: %s\n", module->name);

        if (module->cleanup)
        {
            module->cleanup();
            printf("  [OK] %s cleaned up\n", module->name);
        }

        // Free module memory
        g_free((gpointer)module->name);
        g_free((gpointer)module->xml_path);
        
        // Destroy message queue if exists
        if (module->mq)
        {
            nn_mq_destroy(module->mq);
        }
        
        g_free(module);
    }

    // Destroy hash table
    g_hash_table_destroy(g_module_registry);
    g_module_registry = NULL;

    printf("\nModule cleanup complete\n");
}

// Helper: Set module message queue
void nn_module_set_mq(nn_dev_module_t *module, nn_module_mq_t *mq)
{
    if (module)
    {
        module->mq = mq;
    }
}

// Helper: Get module message queue
nn_module_mq_t *nn_module_get_mq(nn_dev_module_t *module)
{
    return module ? module->mq : NULL;
}

// Request shutdown
void nn_request_shutdown(void)
{
    g_shutdown_requested = 1;
}

// Check if shutdown was requested
int nn_shutdown_requested(void)
{
    return g_shutdown_requested;
}

// Create a message
nn_dev_message_t *nn_message_create(const char *type, void *data, size_t data_len, void (*free_fn)(void *))
{
    nn_dev_message_t *msg = g_malloc(sizeof(nn_dev_message_t));
    if (!msg)
    {
        return NULL;
    }

    msg->type = g_strdup(type);
    msg->data = data;
    msg->data_len = data_len;
    msg->free_fn = free_fn;

    return msg;
}

// Free a message
void nn_message_free(nn_dev_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    g_free(msg->type);

    if (msg->data && msg->free_fn)
    {
        msg->free_fn(msg->data);
    }

    g_free(msg);
}

// Create module message queue
nn_module_mq_t *nn_mq_create(void)
{
    nn_module_mq_t *mq = g_malloc(sizeof(nn_module_mq_t));
    if (!mq)
    {
        return NULL;
    }

    // Create eventfd for notifications
    mq->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (mq->eventfd < 0)
    {
        perror("eventfd");
        g_free(mq);
        return NULL;
    }

    // Create message queue
    mq->message_queue = g_queue_new();
    g_mutex_init(&mq->queue_mutex);

    return mq;
}

// Destroy module message queue
void nn_mq_destroy(nn_module_mq_t *mq)
{
    if (!mq)
    {
        return;
    }

    // Close eventfd
    if (mq->eventfd >= 0)
    {
        close(mq->eventfd);
    }

    // Clear and free message queue
    g_mutex_lock(&mq->queue_mutex);

    while (!g_queue_is_empty(mq->message_queue))
    {
        nn_dev_message_t *msg = g_queue_pop_head(mq->message_queue);
        nn_message_free(msg);
    }

    g_queue_free(mq->message_queue);
    g_mutex_unlock(&mq->queue_mutex);

    g_mutex_clear(&mq->queue_mutex);
    g_free(mq);
}

// Send message to module queue (thread-safe)
int nn_mq_send(nn_module_mq_t *mq, nn_dev_message_t *msg)
{
    if (!mq || !msg)
    {
        return -1;
    }

    // Add message to queue
    g_mutex_lock(&mq->queue_mutex);
    g_queue_push_tail(mq->message_queue, msg);
    g_mutex_unlock(&mq->queue_mutex);

    // Notify via eventfd
    uint64_t val = 1;
    if (write(mq->eventfd, &val, sizeof(val)) != sizeof(val))
    {
        perror("eventfd write");
        return -1;
    }

    return 0;
}

// Receive message from queue (non-blocking, thread-safe)
nn_dev_message_t *nn_mq_receive(nn_module_mq_t *mq)
{
    if (!mq)
    {
        return NULL;
    }

    g_mutex_lock(&mq->queue_mutex);

    nn_dev_message_t *msg = NULL;
    if (!g_queue_is_empty(mq->message_queue))
    {
        msg = g_queue_pop_head(mq->message_queue);

        // Clear eventfd if queue is now empty
        if (g_queue_is_empty(mq->message_queue))
        {
            uint64_t val;
            read(mq->eventfd, &val, sizeof(val));
        }
    }

    g_mutex_unlock(&mq->queue_mutex);

    return msg;
}

// Wait for message on eventfd (blocking with timeout)
int nn_mq_wait(nn_module_mq_t *mq, int timeout_ms)
{
    if (!mq || mq->eventfd < 0)
    {
        return -1;
    }

    struct pollfd pfd;
    pfd.fd = mq->eventfd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);

    if (ret > 0 && (pfd.revents & POLLIN))
    {
        return 1; // Message available
    }
    else if (ret == 0)
    {
        return 0; // Timeout
    }
    else
    {
        return -1; // Error
    }
}

// Get eventfd for external polling
int nn_mq_get_eventfd(nn_module_mq_t *mq)
{
    return mq ? mq->eventfd : -1;
}