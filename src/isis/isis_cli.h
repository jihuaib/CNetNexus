/**
 * @file   isis_cli.h
 * @brief  ISIS CLI 配置命令处理（IPC 线程）
 * @author jhb
 * @date   2026/04/11
 */
#ifndef ISIS_CLI_H
#define ISIS_CLI_H

#include "dev.h"

#define ISIS_CLI_GROUP_ID_INSTANCE 1
#define ISIS_CLI_GROUP_ID_NET 2
#define ISIS_CLI_GROUP_ID_IS_TYPE 3
#define ISIS_CLI_GROUP_ID_AF 4
#define ISIS_CLI_GROUP_ID_IF_VIEW 5
#define ISIS_CLI_GROUP_ID_SHOW_SUMMARY 6
#define ISIS_CLI_GROUP_ID_SHOW_IF 7
#define ISIS_CLI_GROUP_ID_SHOW_NEIGHBOR 8
#define ISIS_CLI_GROUP_ID_SHOW_LSDB 9
#define ISIS_CLI_GROUP_ID_SHOW_ROUTE 10
#define ISIS_CLI_GROUP_ID_COST_STYLE 11
#define ISIS_CLI_GROUP_ID_SRV6_LOCATOR 12

int isis_cli_handle_config_msg(dev_ipc_message_t *msg);

#endif /* ISIS_CLI_H */
