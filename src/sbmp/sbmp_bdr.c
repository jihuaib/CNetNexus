/**
 * @file   sbmp_bdr.c
 * @brief  SBMP 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/03/08
 */
#include "sbmp_bdr.h"

#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "sbmp_db.h"
#include "sbmp_main.h"

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 向请求方发送配置文本响应
 */
static void send_config_resp(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_SBMP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_sbmp_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 公共 API
// ============================================================================

void sbmp_bdr_show_config(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = g_sbmp_local->dev_ipc_ctx;
    char buf[CLI_MAX_RESP_LEN];
    size_t off = 0;

    /* 查询 sbmp_server 表 */
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, SBMP_TABLE_SERVER, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result ||
        result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        /* 无配置，返回空字符串 */
        send_config_resp(msg, "");
        return;
    }

    db_row_t *row = result->rows[0];
    int64_t port = db_row_get_int(row, "server_port", 0);
    db_result_free(result);

    if (port <= 0)
    {
        send_config_resp(msg, "");
        return;
    }

    CLI_BUF_APPEND(buf, sizeof(buf), off, "!\r\n");
    CLI_BUF_APPEND(buf, sizeof(buf), off, "bmp-server\r\n");
    CLI_BUF_APPEND(buf, sizeof(buf), off, " server port %ld\r\n", port);
    CLI_BUF_APPEND(buf, sizeof(buf), off, "!\r\n");

    send_config_resp(msg, buf);
}
