/**
 * @file   bgp_show_route.h
 * @brief  BGP show route 命令入口（实现见 bgp_show_route.c）
 * @author jhb
 * @date   2026/06/01
 */
#ifndef BGP_SHOW_ROUTE_H
#define BGP_SHOW_ROUTE_H

#include "cli.h"
#include "dev.h"

/**
 * @brief 处理 show bgp route af ... 命令（含 vpnv4 按 RD 分组 / RD 过滤）
 * @param msg    CLI 请求消息
 * @param parser 已初始化的 TLV 解析器
 * @return ERRCODE_SUCCESS / ERRCODE_FAIL
 */
int handle_bgp_show_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser);

#endif /* BGP_SHOW_ROUTE_H */
