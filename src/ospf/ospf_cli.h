/**
 * @file   ospf_cli.h
 * @brief  OSPFv2 CLI command handlers
 */
#ifndef OSPF_CLI_H
#define OSPF_CLI_H

#include "dev.h"

#define OSPF_CLI_GROUP_INSTANCE 1u
#define OSPF_CLI_GROUP_ROUTER_ID 2u
#define OSPF_CLI_GROUP_INTERFACE 3u
#define OSPF_CLI_GROUP_SHOW_SUMMARY 4u
#define OSPF_CLI_GROUP_SHOW_INTERFACE 5u
#define OSPF_CLI_GROUP_SHOW_NEIGHBOR 6u
#define OSPF_CLI_GROUP_SHOW_LSDB 7u
#define OSPF_CLI_GROUP_SHOW_ROUTE 8u
#define OSPF_CLI_GROUP_AREA 9u

int ospf_cli_handle_config_msg(dev_ipc_message_t *msg);

#endif /* OSPF_CLI_H */
