/**
 * @file   bgp_bdr.h
 * @brief  BGP 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/03/04
 */
#ifndef BGP_BDR_H
#define BGP_BDR_H

#include "dev.h"

/**
 * @brief 响应 show current-configuration 请求，从 DB 读取 BGP 配置并回复
 * @param msg 原始请求消息（携带 src_module_id 和 request_id）
 */
void bgp_bdr_show_config(dev_ipc_message_t *msg);

#endif /* BGP_BDR_H */
