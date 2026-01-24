#ifndef NN_CLI_DISPATCH_H
#define NN_CLI_DISPATCH_H

#include "nn_cli_tree.h"
#include "nn_dev.h"

// Dispatch command to module via message queue
// If node has module_name set, sends a message to that module's queue
// Returns 0 on success, -1 on error
int nn_cli_dispatch_to_module(nn_cli_tree_node_t *node, const char *cmd_line, const char *args);

#endif // NN_CLI_DISPATCH_H
