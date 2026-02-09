/**
 * @file   nn_cfg_template_renderer.h
 * @brief  配置模板渲染器 - 用于 "show current-configuration" 命令
 * @author jhb
 * @date   2026/01/31
 */
#ifndef NN_CFG_TEMPLATE_RENDERER_H
#define NN_CFG_TEMPLATE_RENDERER_H

#include <glib.h>

// ============================================================================
// 模板渲染 API
// ============================================================================

/**
 * @brief 渲染所有已注册的配置模板
 *
 * 遍历所有已注册的模板，查询对应的数据库，构建变量映射表，
 * 然后渲染模板并组合结果。
 *
 * @return 渲染后的完整配置字符串（调用者负责 g_free），失败返回 NULL
 */
char *nn_cfg_template_renderer_render_all(void);

/**
 * @brief 渲染指定的配置模板及其子模板
 *
 * @param template_name 模板名称
 * @return 渲染后的配置字符串（调用者负责 g_free），未找到返回 NULL
 */
char *nn_cfg_template_renderer_render_by_name(const char *template_name);

#endif // NN_CFG_TEMPLATE_RENDERER_H
