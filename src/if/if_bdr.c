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
#include "if_main.h"
#include "log.h"

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 向请求方发送配置文本响应
 * @param msg  原始请求消息
 * @param text 配置文本（空字符串表示该模块无配置）
 */
static void send_config_resp(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_IF, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_if_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

/**
 * @brief 追加单个接口配置块（if GE-x / ip address / shutdown / !）
 *
 * 仅在接口存在非默认配置时输出（有 IP 或已关闭）。
 *
 * @param buf        输出缓冲区
 * @param buf_size   缓冲区大小
 * @param off        当前写入偏移量（in/out）
 * @param name       逻辑接口名（如 "GE-1"）
 * @param ip_str     IP 地址字符串（空字符串表示未配置）
 * @param prefix_len 前缀长度
 * @param shutdown   1=已关闭，0=正常
 */
static void bdr_append_interface(char *buf, size_t buf_size, size_t *off, const char *name, const char *ip_str,
                                 int64_t prefix_len, int64_t shutdown)
{
    gboolean has_ip = (ip_str && ip_str[0] != '\0');
    gboolean is_shutdown = (shutdown != 0);

    CLI_BUF_APPEND(buf, buf_size, *off, "!\r\n");
    CLI_BUF_APPEND(buf, buf_size, *off, "if %s\r\n", name);

    if (has_ip)
    {
        CLI_BUF_APPEND(buf, buf_size, *off, " ip address %s %ld\r\n", ip_str, prefix_len);
    }

    if (is_shutdown)
    {
        CLI_BUF_APPEND(buf, buf_size, *off, " shutdown\r\n");
    }

    CLI_BUF_APPEND(buf, buf_size, *off, "!\r\n");
}

// ============================================================================
// 公共 API
// ============================================================================

void if_bdr_show_config(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = g_if_local->dev_ipc_ctx;
    char buf[CLI_MAX_RESP_LEN];
    size_t off = 0;
    buf[0] = '\0';

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, "if_interface", NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        send_config_resp(msg, "");
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

        bdr_append_interface(buf, sizeof(buf), &off, name, ip_str, prefix_len, shutdown);
    }

    db_result_free(result);
    send_config_resp(msg, buf);
}
