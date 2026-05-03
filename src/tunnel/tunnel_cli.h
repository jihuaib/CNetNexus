#ifndef TUNNEL_CLI_H
#define TUNNEL_CLI_H

#include "dev.h"

#define TUNNEL_CLI_GROUP_ID_SHOW 1 /**< show tunnel [label] */

int tunnel_cli_handle_show(dev_ipc_message_t *msg);
int tunnel_cli_handle_continue(dev_ipc_message_t *msg);

#endif /* TUNNEL_CLI_H */
