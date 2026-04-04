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
#include "net_addr.h"

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 追加单个接口配置块（if GE-x / ip address / ipv6 address / shutdown / !）
 *
 * 仅在接口存在非默认配置时输出（有 IP 或已关闭）。
 *
 * @param out        输出文本
 * @param name       逻辑接口名（如 "GE-1"）
 * @param ip4_str     IPv4 地址字符串（空字符串表示未配置）
 * @param prefix4_len IPv4 前缀长度
 * @param ip6_str     IPv6 地址字符串（空字符串表示未配置）
 * @param prefix6_len IPv6 前缀长度
 * @param shutdown   1=已关闭，0=正常
 */
static gboolean bdr_addr_is_valid(const char *ip_str, int64_t prefix_len, sa_family_t family)
{
    if (!ip_str || ip_str[0] == '\0')
    {
        return FALSE;
    }

    if ((family == AF_INET && (prefix_len < 0 || prefix_len > 32)) ||
        (family == AF_INET6 && (prefix_len < 0 || prefix_len > 128)))
    {
        return FALSE;
    }

    net_addr_t addr;
    if (net_addr_from_str(ip_str, &addr) != 0 || addr.family != family)
    {
        return FALSE;
    }

    return TRUE;
}

static void bdr_append_interface(GString *out, const char *name, const char *ip4_str, int64_t prefix4_len,
                                 const char *ip6_str, int64_t prefix6_len, int64_t shutdown)
{
    gboolean has_ip4 = bdr_addr_is_valid(ip4_str, prefix4_len, AF_INET);
    gboolean has_ip6 = bdr_addr_is_valid(ip6_str, prefix6_len, AF_INET6);
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

    if (has_ip4)
    {
        g_string_append_printf(out, " ip address %s %ld\r\n", ip4_str, prefix4_len);
    }
    if (has_ip6)
    {
        g_string_append_printf(out, " ipv6 address %s %ld\r\n", ip6_str, prefix6_len);
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
        const char *ip4_str = db_row_get_text(row, "ip_address", NULL);
        int64_t prefix4_len = db_row_get_int(row, "prefix_len", 0);
        const char *ip6_str = db_row_get_text(row, "ipv6_address", NULL);
        int64_t prefix6_len = db_row_get_int(row, "ipv6_prefix_len", 0);
        int64_t shutdown = db_row_get_int(row, "shutdown", 0);

        if (!name)
        {
            continue;
        }

        bdr_append_interface(out, name, ip4_str, prefix4_len, ip6_str, prefix6_len, shutdown);
    }

    db_result_free(result);
    (void)if_cli_send_chunked_response(msg, out);
}
