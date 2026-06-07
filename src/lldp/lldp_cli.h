/**
 * @file   lldp_cli.h
 * @brief  LLDP CLI 配置命令处理
 * @author jhb
 * @date   2026/06/07
 */
#ifndef LLDP_CLI_H
#define LLDP_CLI_H

#include "dev.h"

#define LLDP_CLI_GROUP_ID_PROTO 1
#define LLDP_CLI_GROUP_ID_IF_VIEW 2
#define LLDP_CLI_GROUP_ID_SHOW 3

int lldp_cli_handle_config_msg(dev_ipc_message_t *msg);

#endif /* LLDP_CLI_H */
