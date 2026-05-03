/**
 * @file   tunnel_show.h
 * @brief  Tunnel module show command handling on worker thread.
 */
#ifndef TUNNEL_SHOW_H
#define TUNNEL_SHOW_H

#include "dev.h"

int tunnel_show_dispatch(dev_ipc_message_t *msg);
void tunnel_show_cleanup_state(void);

#endif /* TUNNEL_SHOW_H */
