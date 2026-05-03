/**
 * @file   fib_main.h
 * @brief  FIB 模块主入口头文件
 */
#ifndef FIB_MAIN_H
#define FIB_MAIN_H

#include "dev.h"

typedef struct fib_local
{
    dev_ipc_context_t *dev_ipc_ctx;
} fib_local_t;

extern fib_local_t *g_fib_local;

static inline dev_ipc_context_t *fib_local_ipc_ctx(void)
{
    return g_fib_local->dev_ipc_ctx;
}

void fib_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int fib_module_init(void);
void fib_module_cleanup(void);

#endif /* FIB_MAIN_H */
