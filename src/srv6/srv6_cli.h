#ifndef SRV6_CLI_H
#define SRV6_CLI_H

#include "dev.h"

#define SRV6_CLI_GROUP_PROTOCOL 1u
#define SRV6_CLI_GROUP_LOCATOR 2u
#define SRV6_CLI_GROUP_SHOW 3u

int srv6_cli_handle_config_msg(dev_ipc_message_t *msg);
void srv6_cli_send_response(dev_ipc_message_t *msg, uint32_t response_type, const char *text);

#endif /* SRV6_CLI_H */
