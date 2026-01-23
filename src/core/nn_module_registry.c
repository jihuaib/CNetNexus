#include "nn_module_registry.h"

#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global module registry (GLib hash table: name -> nn_module_t*)
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
void nn_register_module(const char *name, const char *xml_path, nn_module_init_fn init, nn_module_cleanup_fn cleanup)
{
    if (!name)
    {
        return;
    }

    ensure_registry_initialized();

    nn_module_t *module = (nn_module_t *)g_malloc(sizeof(nn_module_t));
    if (!module)
    {
        fprintf(stderr, "Failed to allocate memory for module: %s\n", name);
        return;
    }

    module->name = g_strdup(name);
    module->xml_path = xml_path ? g_strdup(xml_path) : NULL;
    module->init = init;
    module->cleanup = cleanup;
    module->next = NULL; // Keep for backward compatibility
    module->mq = NULL;   // Message queue not initialized yet

    // Add to hash table for fast lookup
    g_hash_table_insert(g_module_registry, (gpointer)module->name, module);

    printf("Registered module: %s\n", name);
}

// Get module by name (O(1) lookup)
nn_module_t *nn_get_module(const char *name)
{
    if (!g_module_registry || !name)
    {
        return NULL;
    }
    return (nn_module_t *)g_hash_table_lookup(g_module_registry, name);
}

// Get the head of the module registry (for backward compatibility)
// Returns first module found (order not guaranteed)
nn_module_t *nn_get_modules(void)
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
        return (nn_module_t *)value;
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
        nn_module_t *module = (nn_module_t *)value;
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
        nn_module_t *module = (nn_module_t *)value;
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
void nn_module_set_mq(nn_module_t *module, nn_module_mq_t *mq)
{
    if (module)
    {
        module->mq = mq;
    }
}

// Helper: Get module message queue
nn_module_mq_t *nn_module_get_mq(nn_module_t *module)
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
