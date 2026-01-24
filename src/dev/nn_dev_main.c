#include <stdint.h>
#include <stdio.h>

#include "nn_cfg.h"
#include "nn_dev.h"
#include "nn_dev_module.h"
#include "nn_errcode.h"

// Forward declarations of dev command callbacks
extern void cmd_show_version(uint32_t client_fd, const char *args);
extern void cmd_sysname(uint32_t client_fd, const char *args);

// Module initialization
static int32_t dev_module_init(void *data)
{
    nn_dev_mq_info_t info;

    memset(&info, 0, sizeof(info));

    // Create message queue
    int ret = nn_mq_create(data, &info);
    if (ret == NN_ERRCODE_FAIL)
    {
        fprintf(stderr, "[dev] Failed to create message queue\n");
        return NN_ERRCODE_FAIL;
    }

    printf("[dev] Dev module initialized (eventfd=%d)\n", info.fd);
    return NN_ERRCODE_SUCCESS;
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
    nn_cfg_register_module_xml(NN_MODULE_ID_DEV, "../../src/dev/commands.xml");
}
