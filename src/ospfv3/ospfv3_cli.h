/**
 * @file   ospfv3_cli.h
 * @brief  OSPFv3 CLI command handlers
 */
#ifndef OSPFV3_CLI_H
#define OSPFV3_CLI_H

#include "dev.h"

#define OSPFV3_CLI_GROUP_INSTANCE 1u
#define OSPFV3_CLI_GROUP_ROUTER_ID 2u
#define OSPFV3_CLI_GROUP_INTERFACE 3u
#define OSPFV3_CLI_GROUP_SHOW_SUMMARY 4u
#define OSPFV3_CLI_GROUP_SHOW_INTERFACE 5u
#define OSPFV3_CLI_GROUP_SHOW_NEIGHBOR 6u
#define OSPFV3_CLI_GROUP_SHOW_LSDB 7u
#define OSPFV3_CLI_GROUP_SHOW_ROUTE 8u
#define OSPFV3_CLI_GROUP_AREA 9u

int ospfv3_cli_handle_config_msg(dev_ipc_message_t *msg);

#endif /* OSPFV3_CLI_H */
