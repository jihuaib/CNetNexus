/**
 * @file   nn_cfg_show_config.h
 * @brief  "show current-configuration" 命令处理头文件
 * @author jhb
 * @date   2026/01/31
 */
#ifndef NN_CFG_SHOW_CONFIG_H
#define NN_CFG_SHOW_CONFIG_H

#include "nn_cfg.h"

// ============================================================================
// 命令处理 API
// ============================================================================

/**
 * @brief 生成 "show current-configuration" 的输出
 * @return 生成的配置字符串（调用者负责 g_free），失败返回 NULL
 */
char *nn_cfg_renderer_show_current_configuration(void);

#endif // NN_CFG_SHOW_CONFIG_H
