/**
 * @file   if_bdr.c
 * @brief  IF 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/03/08
 */
#include "if_bdr.h"

#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "if_cli.h"
#include "if_main.h"
#include "log.h"

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 追加单个接口配置块（if GE-x / ip address / shutdown / !）
 *
 * 仅在接口存在非默认配置时输出（有 IP 或已关闭）。
 *
 * @param out        输出文本
 * @param name       逻辑接口名（如 "GE-1"）
 * @param ip_str     IP 地址字符串（空字符串表示未配置）
 * @param prefix_len 前缀长度
 * @param shutdown   1=已关闭，0=正常
 */
static void bdr_append_interface(GString *out, const char *name, const char *ip_str, int64_t prefix_len,
                                 int64_t shutdown)
{
    gboolean has_ip = (ip_str && ip_str[0] != '\0');
    gboolean is_shutdown = (shutdown != 0);

    /* loop 接口使用 "if loop <N>" 格式，GE/null0 接口使用 "if <name>" 格式 */
    gboolean is_loop = (strncmp(name, "loop", 4) == 0 && name[4] >= '1' && name[4] <= '9');

    g_string_append(out, "!\r\n");
    if (is_loop)
    {
        g_string_append_printf(out, "if loop %s\r\n", name + 4);
    }
    else
    {
        g_string_append_printf(out, "if %s\r\n", name);
    }

    if (has_ip)
    {
        g_string_append_printf(out, " ip address %s %ld\r\n", ip_str, prefix_len);
    }

    if (is_shutdown)
    {
        g_string_append(out, " shutdown\r\n");
    }

    g_string_append(out, "!\r\n");
}

// ============================================================================
// 公共 API
// ============================================================================

void if_bdr_show_config(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    GString *out = g_string_new("");
    if (!out)
    {
        (void)if_cli_send_chunked_response(msg, NULL);
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, "if_interface", NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        (void)if_cli_send_chunked_response(msg, out);
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *name = db_row_get_text(row, "name", NULL);
        const char *ip_str = db_row_get_text(row, "ip_address", NULL);
        int64_t prefix_len = db_row_get_int(row, "prefix_len", 0);
        int64_t shutdown = db_row_get_int(row, "shutdown", 0);

        if (!name)
        {
            continue;
        }

        bdr_append_interface(out, name, ip_str, prefix_len, shutdown);
    }

    db_result_free(result);
    (void)if_cli_send_chunked_response(msg, out);
}
