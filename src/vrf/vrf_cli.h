/**
 * @file   vrf_cli.h
 * @brief  VRF 模块 CLI 命令处理声明
 * @author jhb
 * @date   2026/03/05
 */
#ifndef VRF_CLI_H
#define VRF_CLI_H

#include "dev.h"

/**
 * @brief 处理 CLI 命令消息（CLI_MSG_TYPE）
 * @param msg 原始 IPC 消息
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int vrf_cli_handle_message(dev_ipc_message_t *msg);

/**
 * @brief 处理 CLI 分块继续请求（CLI_MSG_TYPE_CONTINUE）
 * @param msg 原始 IPC 消息
 * @return 成功返回 ERRCODE_SUCCESS
 */
int vrf_cli_handle_continue(dev_ipc_message_t *msg);

/**
 * @brief 处理动态候选值查询（CLI_MSG_TYPE_QUERY_CANDIDATES）
 *
 * payload 为 uint32_t cfg_id（网络字节序）：
 *   cfg_id=3 → 返回所有 VRF 名称
 *   cfg_id=4 → 返回可删除的 VRF 名称（排除公网 VRF）
 *
 * @param ctx IPC 上下文
 * @param msg 原始 IPC 消息（函数负责释放）
 */
void vrf_cli_handle_query_candidates(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

#endif /* VRF_CLI_H */
