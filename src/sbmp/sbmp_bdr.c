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
#include "sbmp_cli.h"
#include "sbmp_db.h"
#include "sbmp_main.h"

// ============================================================================
// 公共 API
// ============================================================================

void sbmp_bdr_show_config(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = sbmp_local_ipc_ctx();
    GString *out = g_string_new("");
    if (!out)
    {
        (void)sbmp_cli_send_chunked_response(msg, NULL);
        return;
    }

    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        LOG_WARN("SBMP BDR: invalid SHOW_CONFIG scope payload");
        (void)sbmp_cli_send_chunked_response(msg, out);
        return;
    }
    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS && strcmp(scope.view_name, CLI_VIEW_SBMP) != 0)
    {
        (void)sbmp_cli_send_chunked_response(msg, out);
        return;
    }

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
        (void)sbmp_cli_send_chunked_response(msg, out);
        return;
    }

    db_row_t *row = result->rows[0];
    int64_t port = db_row_get_int(row, "server_port", 0);
    db_result_free(result);

    if (port <= 0)
    {
        (void)sbmp_cli_send_chunked_response(msg, out);
        return;
    }

    g_string_append(out, "!\r\n");
    g_string_append(out, "bmp-server\r\n");
    g_string_append_printf(out, " server port %ld\r\n", port);
    g_string_append(out, "!\r\n");

    (void)sbmp_cli_send_chunked_response(msg, out);
}
