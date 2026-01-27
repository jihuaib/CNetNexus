#include "nn_dev_module.h"

#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_dev.h"
#include "nn_errcode.h"

// Global module registry (GLib tree: id -> nn_dev_module_t*)
static GTree *g_module_registry = NULL;

// Comparison function for GTree keys (module_id)
static gint module_id_compare(gconstpointer a, gconstpointer b)
{
    uint32_t id_a = GPOINTER_TO_UINT(a);
    uint32_t id_b = GPOINTER_TO_UINT(b);

    if (id_a < id_b)
    {
        return -1;
    }
    if (id_a > id_b)
    {
        return 1;
    }
    return 0;
}

// Global shutdown flag
static volatile sig_atomic_t g_shutdown_requested = 0;

// Initialize module registry
static void ensure_registry_initialized(void)
{
    if (!g_module_registry)
    {
        g_module_registry = g_tree_new(module_id_compare);
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

    nn_dev_module_t *module = (nn_dev_module_t *)g_malloc0(sizeof(nn_dev_module_t));

    module->module_id = id;
    strlcpy(module->name, name, sizeof(module->name));
    module->init = init;
    module->cleanup = cleanup;
    module->mq = NULL; // Message queue not initialized yet

    // Add to tree for sorted lookup and iteration
    g_tree_insert(g_module_registry, GUINT_TO_POINTER(module->module_id), module);

    printf("[dev] Registered module: %s\n", name);
}

// Request shutdown
void nn_dev_request_shutdown(void)
{
    g_shutdown_requested = 1;
}

// Check if shutdown was requested
int nn_dev_shutdown_requested(void)
{
    return g_shutdown_requested;
}

// Helper for module initialization traversal
static gboolean init_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    int32_t *failed_count = (int32_t *)data;
    nn_dev_module_t *module = (nn_dev_module_t *)value;

    printf("[dev] Initializing module: %s\n", module->name);

    if (module->init)
    {
        if (module->init((void *)module) == NN_ERRCODE_SUCCESS)
        {
            printf("[dev] %s initialized OK\n", module->name);
        }
        else
        {
            fprintf(stderr, "[dev] %s initialization failed\n", module->name);
            (*failed_count)++;
        }
    }
    else
    {
        printf("[dev] %s has no init function\n", module->name);
    }

    return FALSE; // Continue traversal
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

    // Iterate through all modules in-order (by module_id)
    g_tree_foreach(g_module_registry, init_module_callback, &failed_count);

    printf("\n[dev] Module initialization complete (failures: %d)\n\n", failed_count);

    return failed_count;
}

// Helper for module cleanup traversal
static gboolean cleanup_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    (void)data;
    nn_dev_module_t *module = (nn_dev_module_t *)value;

    printf("[dev] Cleaning up module: %s\n", module->name);

    if (module->cleanup)
    {
        module->cleanup();
        printf("[dev]%s cleaned up ok\n", module->name);
    }

    // Destroy message queue if exists
    if (module->mq)
    {
        nn_dev_mq_destroy(module->mq);
    }

    g_free(module);

    return FALSE; // Continue traversal
}

// Cleanup all registered modules
void nn_cleanup_all_modules(void)
{
    printf("\n[dev] Cleaning up modules:\n");
    printf("====================\n");

    if (!g_module_registry)
    {
        printf("[dev] No modules to clean up\n");
        return;
    }

    // Cleanup all modules in-order
    g_tree_foreach(g_module_registry, cleanup_module_callback, NULL);

    // Destroy tree
    g_tree_destroy(g_module_registry);
    g_module_registry = NULL;

    printf("\n[dev] Module cleanup complete\n");
}

int nn_dev_get_module_name(uint32_t module_id, char *module_name)
{
    if (!g_module_registry || !module_name)
    {
        snprintf(module_name, NN_DEV_MODULE_NAME_MAX_LEN, "unknown");
        return NN_ERRCODE_FAIL;
    }

    nn_dev_module_t *module = (nn_dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(module_id));
    if (module)
    {
        strlcpy(module_name, module->name, NN_DEV_MODULE_NAME_MAX_LEN);
        return NN_ERRCODE_SUCCESS;
    }

    return NN_ERRCODE_FAIL;
}