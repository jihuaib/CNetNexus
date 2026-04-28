/**
 * @file   dev_bdr.h
 * @brief  DEV 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/04/27
 */
#ifndef DEV_BDR_H
#define DEV_BDR_H

#include "dev.h"

/**
 * @brief 响应 show current-configuration 请求，从 DB 读取 DEV 配置并回复
 * @param msg 原始请求消息（携带 src_module_id 和 request_id）
 * @details 缺省值（与构建版本对应）不显示，仅显示用户实际修改过的配置项
 */
void dev_bdr_show_config(dev_ipc_message_t *msg);

#endif /* DEV_BDR_H */
