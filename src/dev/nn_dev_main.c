#include "nn_dev.h"

#include <stdint.h>
#include <stdio.h>

// Forward declarations of dev command callbacks
extern void cmd_show_version(uint32_t client_fd, const char *args);
extern void cmd_sysname(uint32_t client_fd, const char *args);

// Module initialization
static int32_t dev_module_init(void)
{
    // Create message queue
    nn_module_mq_t *mq = nn_mq_create();
    if (!mq)
    {
        fprintf(stderr, "[dev] Failed to create message queue\n");
        return -1;
    }
    
    // Register message queue with module
    nn_dev_module_t *self = nn_get_module("dev");
    if (self)
    {
        nn_module_set_mq(self, mq);
    }
    
    printf("[dev] Dev module initialized (eventfd=%d)\n", nn_mq_get_eventfd(mq));
    return 0;
}

// Module cleanup
static void dev_module_cleanup(void)
{
    printf("[dev] Dev module cleanup\n");
}

// Register dev module using constructor attribute
static void __attribute__((constructor)) register_dev_module(void)
{
    nn_dev_register_module(NN_MODULE_ID_DEV, "nn_dev", dev_module_init, dev_module_cleanup);
}
