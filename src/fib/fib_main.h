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

/** FIB 内部消息：VRF 每次 READY（含初次 + 重启）触发 worker 重新订阅 VRF 事件
 *  category=FIB, subtype=0xFFFE */
#define FIB_MSG_TYPE_INTERNAL_VRF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0xFFFE)

void fib_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int fib_module_init(void);
void fib_module_cleanup(void);

#endif /* FIB_MAIN_H */
