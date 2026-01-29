#include "nn_dev_module.h"

#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_dev.h"
#include "nn_dev_mq.h"
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
void nn_dev_register_module_inner(uint32_t id, const char *name, nn_module_init_fn init, nn_module_cleanup_fn cleanup)
{
    ensure_registry_initialized();

    nn_dev_module_t *module = (nn_dev_module_t *)g_malloc0(sizeof(nn_dev_module_t));

    module->module_id = id;
    strlcpy(module->name, name, sizeof(module->name));
    module->init = init;
    module->cleanup = cleanup;
    module->mq = NULL; // Message queue not initialized yet

    // Add to tree for sorted lookup and iteration
    g_tree_insert(g_module_registry, GUINT_TO_POINTER(module->module_id), module);
}

// Request shutdown
void nn_dev_request_shutdown_inner(void)
{
    g_shutdown_requested = 1;
}

// Check if shutdown was requested
int nn_dev_shutdown_requested_inner(void)
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
static gboolean cleanup_module_callback(nn_dev_module_t *module)
{
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

// Helper to collect modules into a list for reverse traversal
static gboolean collect_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    GList **list = (GList **)data;
    *list = g_list_prepend(*list, value);
    return FALSE;
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

    // Collect all modules, then iterate in reverse order (highest ID first)
    // g_tree_foreach visits in ascending order, g_list_prepend reverses it
    GList *modules = NULL;
    g_tree_foreach(g_module_registry, collect_module_callback, &modules);

    for (GList *l = modules; l != NULL; l = l->next)
    {
        nn_dev_module_t *module = (nn_dev_module_t *)l->data;
        (void)cleanup_module_callback(module);
    }

    g_list_free(modules);

    // Destroy tree (nodes already freed above, so use g_tree_destroy without value_destroy_func)
    g_tree_destroy(g_module_registry);
    g_module_registry = NULL;

    printf("\n[dev] Module cleanup complete\n");
}

int nn_dev_get_module_name_inner(uint32_t module_id, char *module_name)
{
    module_name[0] = '\0';

    if (!g_module_registry)
    {
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

void nn_dev_module_foreach(GTraverseFunc func, gpointer user_data)
{
    if (g_module_registry)
    {
        g_tree_foreach(g_module_registry, func, user_data);
    }
}