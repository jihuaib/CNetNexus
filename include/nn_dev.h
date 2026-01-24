#ifndef NN_DEV_H
#define NN_DEV_H

#include <glib.h>
#include <stdint.h>

#include "dev/nn_dev_mq.h"

#define NN_MODULE_ID_CFG 0x0000001
#define NN_MODULE_ID_DEV 0x0000002
#define NN_MODULE_ID_BGP 0x0000003

#define NN_DEV_INVALID_FD (-1)

typedef struct nn_dev_mq_info
{
    int fd;
    void *data;
} nn_dev_mq_info_t;

// Module initialization callback type
// Returns 0 on success, non-zero on failure
typedef int32_t (*nn_module_init_fn)(void *module);

// Module cleanup callback type
typedef void (*nn_module_cleanup_fn)(void);

// Register a module with init/cleanup callbacks
void nn_dev_register_module(uint32_t id, const char *name, nn_module_init_fn init, nn_module_cleanup_fn cleanup);

// Request shutdown of all modules
void nn_request_shutdown(void);

// Check if shutdown was requested
int nn_shutdown_requested(void);

// Create a message
nn_dev_message_t *nn_message_create(const char *type, void *data, size_t data_len, void (*free_fn)(void *));

// Free a message
void nn_message_free(nn_dev_message_t *msg);

// Create module message queue
int nn_mq_create(void *data, nn_dev_mq_info_t *info);

void nn_mq_destroy(nn_dev_mq_info_t *info);

#endif // NN_DEV_H
