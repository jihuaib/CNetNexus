/**
 * @file   dev_bdr.c
 * @brief  DEV 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/04/27
 */
#include "dev_bdr.h"

#include <glib.h>

#include "cli.h"
#include "dev_db.h"
#include "dev_main.h"
#include "log.h"

/** log_level 数值 -> 命令行 keyword */
static const char *log_level_keyword(log_level_t level)
{
    switch (level)
    {
        case LOG_LEVEL_DEBUG:
            return "debug";
        case LOG_LEVEL_INFO:
            return "info";
        case LOG_LEVEL_WARN:
            return "warn";
        case LOG_LEVEL_ERROR:
            return "error";
        default:
            return NULL;
    }
}

void dev_bdr_show_config(dev_ipc_message_t *msg)
{
    GString *out = g_string_new("");

    log_level_t level;
    int rc = dev_db_get_log_level(&level);
    if (rc == 0 && level != dev_db_default_log_level())
    {
        const char *kw = log_level_keyword(level);
        if (kw)
        {
            g_string_append(out, "!\r\n");
            g_string_append_printf(out, "dev log log-level %s\r\n", kw);
        }
    }
    /* rc != 0 或 level == default：不显示 */

    (void)cli_chunk_stream_start(&g_dev_local->show_stream, dev_get_ipc_ctx(), DEV_MODULE_ID_DEV, msg, out);
}
