#ifndef TUNNEL_MAIN_H
#define TUNNEL_MAIN_H

#include "dev.h"

typedef struct tunnel_local
{
    dev_ipc_context_t *dev_ipc_ctx;
} tunnel_local_t;

extern tunnel_local_t *g_tunnel_local;

static inline dev_ipc_context_t *tunnel_local_ipc_ctx(void)
{
    return g_tunnel_local->dev_ipc_ctx;
}

void tunnel_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int tunnel_module_init(void);
void tunnel_module_cleanup(void);

#endif /* TUNNEL_MAIN_H */
