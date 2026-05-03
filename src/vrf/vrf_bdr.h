/**
 * @file   vrf_bdr.h
 * @brief  VRF 配置 backed-by-DB 输出（CLI_MSG_TYPE_SHOW_CONFIG）
 * @author jhb
 * @date   2026/05/02
 */
#ifndef VRF_BDR_H
#define VRF_BDR_H

#include "dev.h"

/**
 * @brief 处理 SHOW_CONFIG 请求：从 DB 生成 VRF 配置文本
 */
void vrf_bdr_show_config(dev_ipc_message_t *msg);

#endif /* VRF_BDR_H */
