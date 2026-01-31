/**
 * @file   nn_cfg_show_config.c
 * @brief  "show current-configuration" 命令处理实现
 * @author jhb
 * @date   2026/01/31
 */
#include "nn_cfg_show_config.h"

#include "nn_cfg_template_renderer.h"

// ============================================================================
// 命令处理实现
// ============================================================================

char *nn_cfg_renderer_show_current_configuration(void)
{
    // 直接调用模板渲染器
    return nn_cfg_template_renderer_render_all();
}
