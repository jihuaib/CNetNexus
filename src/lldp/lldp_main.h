/**
 * @file   lldp_main.h
 * @brief  LLDP 模块主入口头文件
 * @author jhb
 * @date   2026/06/07
 */
#ifndef LLDP_MAIN_H
#define LLDP_MAIN_H

#include "dev.h"

typedef struct lldp_local
{
    dev_ipc_context_t *dev_ipc_ctx;
} lldp_local_t;

extern lldp_local_t *g_lldp_local;

static inline dev_ipc_context_t *lldp_local_ipc_ctx(void)
{
    return g_lldp_local ? g_lldp_local->dev_ipc_ctx : NULL;
}

#define LLDP_MSG_TYPE_INTERNAL_IF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_LLDP, 0xFFFE)
#define LLDP_MSG_TYPE_INTERNAL_IF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_LLDP, 0xFFFD)
#define LLDP_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_LLDP, 0xFFFC)
#define LLDP_MSG_TYPE_INTERNAL_SNMP_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_LLDP, 0xFFFB)

void lldp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int lldp_module_init(void);
void lldp_module_cleanup(void);

#endif /* LLDP_MAIN_H */
