#include "nn_dev_module.h"

#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_dev.h"
#include "nn_errcode.h"

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

    module->module_id = id;
    strlcpy(module->name, name, sizeof(module->name));
    module->init = init;
    module->cleanup = cleanup;
    module->mq = NULL; // Message queue not initialized yet

    // Add to hash table for fast lookup
    g_hash_table_insert(g_module_registry, (gpointer)module->name, module);

    printf("[dev]Registered module: %s\n", name);
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

// Initialize all registered modules
int32_t nn_dev_init_all_modules(void)
{
    int32_t failed_count = 0;

    printf("\nInitializing modules:\n");
    printf("=====================\n");

    if (!g_module_registry)
    {
        printf("No modules registered\n");
        return NN_ERRCODE_SUCCESS;
    }

    // Iterate through all modules using GHashTable iterator
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_hash_table_iter_init(&iter, g_module_registry);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        nn_dev_module_t *module = (nn_dev_module_t *)value;
        printf("[dev]Initializing module: %s\n", module->name);

        if (module->init)
        {
            if (module->init((void *)module) == NN_ERRCODE_SUCCESS)
            {
                printf("[dev]%s initialized OK\n", module->name);
            }
            else
            {
                fprintf(stderr, "[dev]%s initialization failed\n", module->name);
                failed_count++;
            }
        }
        else
        {
            printf("[dev]%s has no init function\n", module->name);
        }
    }

    printf("\n[dev]Module initialization complete (failures: %d)\n\n", failed_count);

    return failed_count;
}

// Cleanup all registered modules
void nn_cleanup_all_modules(void)
{
    printf("\n[dev]Cleaning up modules:\n");
    printf("====================\n");

    if (!g_module_registry)
    {
        printf("[dev]No modules to clean up\n");
        return;
    }

    // Cleanup all modules using iterator
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_hash_table_iter_init(&iter, g_module_registry);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        nn_dev_module_t *module = (nn_dev_module_t *)value;
        printf("[dev]Cleaning up module: %s\n", module->name);

        if (module->cleanup)
        {
            module->cleanup();
            printf("[dev]%s cleaned up ok\n", module->name);
        }

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

    printf("\n[dev]Module cleanup complete\n");
}