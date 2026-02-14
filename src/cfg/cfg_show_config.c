/**
 * @file   cfg_show_config.c
 * @brief  "show current-configuration" 命令处理实现
 * @author jhb
 * @date   2026/01/31
 */
#include "cfg_show_config.h"

#include <glib.h>

// ============================================================================
// 命令处理实现
// ============================================================================

char *cfg_renderer_show_current_configuration(void)
{
    return g_strdup("show current-configuration: not implemented\r\n");
}
