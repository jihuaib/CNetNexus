/**
 * @file   vrf_cli.h
 * @brief  VRF 模块 CLI 配置命令处理（IPC 线程内解析 TLV，dispatch 给 worker）
 * @author jhb
 * @date   2026/03/05
 */
#ifndef VRF_CLI_H
#define VRF_CLI_H

#include "dev.h"

/**
 * @brief 处理 CLI 配置命令（show 命令由 vrf_msg_handler 转 worker，不走此函数）
 */
int vrf_cli_handle_config_msg(dev_ipc_message_t *msg);

#endif /* VRF_CLI_H */
