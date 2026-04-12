/**
 * @file   isis_main.h
 * @brief  ISIS 模块主入口头文件
 * @author jhb
 * @date   2026/04/11
 */
#ifndef ISIS_MAIN_H
#define ISIS_MAIN_H

#include "dev.h"

typedef struct isis_local
{
    dev_ipc_context_t *dev_ipc_ctx;
} isis_local_t;

extern isis_local_t *g_isis_local;

static inline dev_ipc_context_t *isis_local_ipc_ctx(void)
{
    return g_isis_local->dev_ipc_ctx;
}

void isis_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int isis_module_init(void);
void isis_module_cleanup(void);

#endif /* ISIS_MAIN_H */
