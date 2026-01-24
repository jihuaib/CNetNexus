#include "nn_cli_dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dev/nn_dev_module.h"
#include "nn_errcode.h"

// Dispatch command to module via message queue
int nn_cli_dispatch_to_module(nn_cli_tree_node_t *node, const char *cmd_line, const char *args)
{
    if (!node || !node->module_name)
    {
        // No module associated, nothing to dispatch
        return NN_ERRCODE_SUCCESS;
    }

    // Get the target module
    nn_dev_module_t *module = nn_get_module(node->module_name);
    if (!module)
    {
        fprintf(stderr, "[dispatch] Module '%s' not found\n", node->module_name);
        return NN_ERRCODE_FAIL;
    }

    // Check if module has message queue
    if (!module->mq)
    {
        fprintf(stderr, "[dispatch] Module '%s' has no message queue\n", node->module_name);
        return NN_ERRCODE_FAIL;
    }

    // Create command message
    // Format: "command_name args"
    size_t msg_len = strlen(cmd_line) + (args ? strlen(args) : 0) + 2;
    char *msg_data = g_malloc(msg_len);

    if (args && strlen(args) > 0)
    {
        snprintf(msg_data, msg_len, "%s %s", cmd_line, args);
    }
    else
    {
        snprintf(msg_data, msg_len, "%s", cmd_line);
    }

    // Create message
    nn_dev_message_t *msg = nn_message_create("command", msg_data, msg_len, g_free);
    if (!msg)
    {
        g_free(msg_data);
        return NN_ERRCODE_FAIL;
    }

    // Send to module's message queue
    if (nn_mq_send(module->mq, msg) != NN_ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[dispatch] Failed to send message to module '%s'\n", node->module_name);
        nn_message_free(msg);
        return NN_ERRCODE_FAIL;
    }

    printf("[dispatch] Command '%s' dispatched to module '%s'\n", cmd_line, node->module_name);
    return NN_ERRCODE_SUCCESS;
}
