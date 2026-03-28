/**
 * @file   route_bdr.h
 * @brief  Route 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/03/28
 */
#ifndef ROUTE_BDR_H
#define ROUTE_BDR_H

#include "dev.h"

/**
 * @brief 处理 show current-configuration 请求（读取 DB 生成配置文本）
 * @param msg 原始请求消息（携带 src_module_id 和 request_id）
 * @return ERRCODE_SUCCESS 成功
 */
int route_bdr_handle_show_config(dev_ipc_message_t *msg);

#endif /* ROUTE_BDR_H */
