/**
 * @file   ldp_cli.h
 * @brief  LDP CLI 配置命令处理（IPC 线程）
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_CLI_H
#define LDP_CLI_H

#include "dev.h"

#define LDP_CLI_GROUP_ID_PROTO 1
#define LDP_CLI_GROUP_ID_LSR_ID 2
#define LDP_CLI_GROUP_ID_TIMERS 3
#define LDP_CLI_GROUP_ID_IF_VIEW 4
#define LDP_CLI_GROUP_ID_SHOW 5

int ldp_cli_handle_config_msg(dev_ipc_message_t *msg);

#endif /* LDP_CLI_H */
