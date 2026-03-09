/**
 * @file   sbmp_bdr.h
 * @brief  SBMP 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/03/08
 */
#ifndef SBMP_BDR_H
#define SBMP_BDR_H

#include "dev.h"

/**
 * @brief 响应 show current-configuration 请求，从 DB 读取 SBMP 配置并回复
 * @param msg 原始请求消息（携带 src_module_id 和 request_id）
 */
void sbmp_bdr_show_config(dev_ipc_message_t *msg);

#endif /* SBMP_BDR_H */
