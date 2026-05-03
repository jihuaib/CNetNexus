/**
 * @file   vrf_show.h
 * @brief  VRF show 命令处理（worker 线程）
 * @author jhb
 * @date   2026/05/02
 */
#ifndef VRF_SHOW_H
#define VRF_SHOW_H

#include "dev.h"

/**
 * @brief 处理 CLI 命令消息（show 命令）
 */
int vrf_show_handle_cli(dev_ipc_message_t *msg);

/**
 * @brief 处理 CLI 分块继续请求
 */
int vrf_show_handle_continue(dev_ipc_message_t *msg);

/**
 * @brief 处理 SHOW_CONFIG 请求（show this / show current-configuration）
 */
int vrf_show_handle_show_config(dev_ipc_message_t *msg);

/**
 * @brief 处理动态候选值查询
 */
void vrf_show_handle_query_candidates(dev_ipc_message_t *msg);

#endif /* VRF_SHOW_H */
