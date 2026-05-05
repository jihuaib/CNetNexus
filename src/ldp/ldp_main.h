/**
 * @file   ldp_main.h
 * @brief  LDP 模块主入口头文件
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_MAIN_H
#define LDP_MAIN_H

#include "dev.h"

typedef struct ldp_local
{
    dev_ipc_context_t *dev_ipc_ctx;
} ldp_local_t;

extern ldp_local_t *g_ldp_local;

static inline dev_ipc_context_t *ldp_local_ipc_ctx(void)
{
    return g_ldp_local ? g_ldp_local->dev_ipc_ctx : NULL;
}

void ldp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int ldp_module_init(void);
void ldp_module_cleanup(void);

#endif /* LDP_MAIN_H */
