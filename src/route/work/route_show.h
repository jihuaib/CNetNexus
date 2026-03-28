/**
 * @file   route_show.h
 * @brief  Route 模块 show 命令处理头文件（在内存 RIB 中查询，仅在 worker 线程访问）
 * @author jhb
 * @date   2026/03/28
 */
#ifndef ROUTE_SHOW_H
#define ROUTE_SHOW_H

#include <glib.h>

#include "cli.h"
#include "dev.h"

/**
 * @brief 处理 show route 命令（显示 RIB 路由表）
 * @param msg    原始请求消息
 * @param parser TLV 解析器（已初始化）
 * @return ERRCODE_SUCCESS 成功
 */
int route_show_handle_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser);

/**
 * @brief 处理 show route relay 命令（显示 nexthop 迭代状态）
 * @param msg    原始请求消息
 * @param parser TLV 解析器（已初始化）
 * @return ERRCODE_SUCCESS 成功
 */
int route_show_handle_relay(dev_ipc_message_t *msg, cli_tlv_parser_t *parser);

/**
 * @brief 处理 show route static 命令（显示候选静态路由表）
 * @param msg    原始请求消息
 * @param parser TLV 解析器（已初始化）
 * @return ERRCODE_SUCCESS 成功
 */
int route_show_handle_static(dev_ipc_message_t *msg, cli_tlv_parser_t *parser);

/**
 * @brief 处理 CLI continue 消息（分片输出的后续片）
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int route_show_handle_continue(dev_ipc_message_t *msg);

/**
 * @brief 清理 show 命令分片流状态
 */
void route_show_cleanup_state(void);

/**
 * @brief 分发 show 命令消息（解析 TLV group_id 后转发到对应 show 处理函数）
 *
 * 由 worker 线程在收到 CLI_SHOW 命令时调用。
 *
 * @param msg 原始请求消息
 * @return ERRCODE_SUCCESS 成功，ERRCODE_FAIL 失败
 */
int route_show_dispatch(dev_ipc_message_t *msg);

/**
 * @brief 发送分片响应（供 route_bdr.c 调用）
 * @param msg       原始请求消息
 * @param full_text 完整输出文本（调用后所有权转移，函数内释放）
 * @return ERRCODE_SUCCESS 成功
 */
int route_show_send_chunked(dev_ipc_message_t *msg, GString *full_text);

#endif /* ROUTE_SHOW_H */
