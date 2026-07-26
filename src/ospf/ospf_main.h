/**
 * @file   ospf_main.h
 * @brief  OSPFv2 module lifecycle
 */
#ifndef OSPF_MAIN_H
#define OSPF_MAIN_H

#include <signal.h>

#include "dev.h"

typedef struct ospf_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    volatile sig_atomic_t shutting_down;
} ospf_local_t;

extern ospf_local_t *g_ospf_local;

static inline dev_ipc_context_t *ospf_local_ipc_ctx(void)
{
    return g_ospf_local ? g_ospf_local->dev_ipc_ctx : NULL;
}

#define OSPF_MSG_TYPE_INTERNAL_IF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_OSPF, 0xFFFE)
#define OSPF_MSG_TYPE_INTERNAL_ROUTE_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_OSPF, 0xFFFD)
#define OSPF_MSG_TYPE_INTERNAL_IF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_OSPF, 0xFFFC)
#define OSPF_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_OSPF, 0xFFFB)

void ospf_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int ospf_module_init(void);
void ospf_module_cleanup(void);

#endif /* OSPF_MAIN_H */
