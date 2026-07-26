/**
 * @file   ospfv3_main.h
 * @brief  OSPFv3 module lifecycle
 */
#ifndef OSPFV3_MAIN_H
#define OSPFV3_MAIN_H

#include <signal.h>

#include "dev.h"

typedef struct ospfv3_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    volatile sig_atomic_t shutting_down;
} ospfv3_local_t;

extern ospfv3_local_t *g_ospfv3_local;

static inline dev_ipc_context_t *ospfv3_local_ipc_ctx(void)
{
    return g_ospfv3_local ? g_ospfv3_local->dev_ipc_ctx : NULL;
}

#define OSPFV3_MSG_TYPE_INTERNAL_IF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_OSPFV3, 0xFFFE)
#define OSPFV3_MSG_TYPE_INTERNAL_ROUTE_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_OSPFV3, 0xFFFD)
#define OSPFV3_MSG_TYPE_INTERNAL_IF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_OSPFV3, 0xFFFC)
#define OSPFV3_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_OSPFV3, 0xFFFB)

void ospfv3_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int ospfv3_module_init(void);
void ospfv3_module_cleanup(void);

#endif /* OSPFV3_MAIN_H */
