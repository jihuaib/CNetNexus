/**
 * @file   snmp_cli.h
 * @brief  SNMP CLI command handling
 */
#ifndef SNMP_CLI_H
#define SNMP_CLI_H

#include <glib.h>

#include "dev.h"

#define SNMP_CLI_GROUP_ID_TRAP_SERVER 1

int snmp_cli_handle_config_msg(dev_ipc_message_t *msg);
void snmp_cli_send_response(dev_ipc_message_t *msg, const char *text);

#endif /* SNMP_CLI_H */
