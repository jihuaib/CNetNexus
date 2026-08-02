#ifndef SRV6_MAIN_H
#define SRV6_MAIN_H

#include <glib.h>
#include <pthread.h>

#include "dev.h"

#define SRV6_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x1001)
#define SRV6_MSG_TYPE_INTERNAL_DB_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x1002)
#define SRV6_MSG_TYPE_INTERNAL_VRF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x1003)
#define SRV6_MSG_TYPE_INTERNAL_VRF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x1004)
#define SRV6_MSG_TYPE_INTERNAL_FIB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x1005)
#define SRV6_MSG_TYPE_INTERNAL_FIB_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x1006)
#define SRV6_MSG_TYPE_INTERNAL_ROUTE_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x1007)
#define SRV6_MSG_TYPE_INTERNAL_ROUTE_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x1008)

typedef struct srv6_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    pthread_mutex_t startup_mutex;
    pthread_cond_t startup_cond;
    gboolean db_ready;
    gboolean fib_ready;
    gboolean route_ready;
    gboolean vrf_smoothend;
    gboolean restore_done;
    int restore_rc;
    gboolean shutting_down;
} srv6_local_t;

extern srv6_local_t *g_srv6_local;

static inline dev_ipc_context_t *srv6_local_ipc_ctx(void)
{
    return g_srv6_local ? g_srv6_local->dev_ipc_ctx : NULL;
}

int srv6_module_init(void);
void srv6_module_cleanup(void);

#endif /* SRV6_MAIN_H */
