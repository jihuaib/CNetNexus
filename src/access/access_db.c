/**
 * @file   access_db.c
 * @brief  ACCESS 配置持久化实现（封装 db_rpc）
 * @author jhb
 * @date   2026/05/30
 */
#include "access_db.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "access.h"
#include "access_line.h"
#include "access_main.h"
#include "db.h"
#include "errcode.h"
#include "log.h"

#define ACCESS_TABLE_SETTING "access_setting"
#define ACCESS_TABLE_LINE "access_line"
#define ACCESS_SETTING_TELNET "telnet_server"

/* access_setting：key-value，存全局开关（telnet_server） */
static const db_column_def_t SETTING_COLS[] = {
    {"skey", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"sval", DB_TYPE_INTEGER, 0, NULL},
};
static const db_table_def_t SETTING_TABLE = {
    .table_name = ACCESS_TABLE_SETTING,
    .cols = SETTING_COLS,
    .num_cols = G_N_ELEMENTS(SETTING_COLS),
};

/* access_line：每条线一行（con0 默认行 + 各 vty 的 transport input） */
static const db_column_def_t LINE_COLS[] = {
    {"line_key", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"line_type", DB_TYPE_INTEGER, 0, NULL},
    {"line_num", DB_TYPE_INTEGER, 0, NULL},
    {"transport", DB_TYPE_INTEGER, 0, NULL},
};
static const db_table_def_t LINE_TABLE = {
    .table_name = ACCESS_TABLE_LINE,
    .cols = LINE_COLS,
    .num_cols = G_N_ELEMENTS(LINE_COLS),
};

/* 插入一行 line（先按 line_key 删再插，实现 upsert） */
static int line_upsert(dev_ipc_context_t *ctx, const char *key, int line_type, uint32_t line_num, uint8_t transport)
{
    db_filter_builder_t fb;
    db_filter_init(&fb);
    db_filter_add_text(&fb, "line_key", key);
    db_rpc_delete(ctx, ACCESS_TABLE_LINE, &fb.filter);
    db_filter_clear(&fb);

    db_col_t cols[] = {
        DB_COL_TEXT("line_key", key),
        DB_COL_INT("line_type", line_type),
        DB_COL_INT("line_num", line_num),
        DB_COL_INT("transport", transport),
    };
    return db_rpc_insert_cols(ctx, ACCESS_TABLE_LINE, cols, G_N_ELEMENTS(cols));
}

int access_db_init(void)
{
    dev_ipc_context_t *ctx = access_ipc_ctx();
    if (db_rpc_create_table_from_def(ctx, &SETTING_TABLE) != ERRCODE_SUCCESS ||
        db_rpc_create_table_from_def(ctx, &LINE_TABLE) != ERRCODE_SUCCESS)
    {
        /* DB 可能尚未就绪，调用方会重试，这里不刷 ERROR */
        return -1;
    }

    /* 首次启动（line 表为空）：写入默认行——console 线 + 各 vty 线（transport=none）。
     * 满足"串口 line 默认写 DB"。 */
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, ACCESS_TABLE_LINE, NULL, 0, NULL, &result) == ERRCODE_SUCCESS)
    {
        int empty = (!result || result->num_rows == 0);
        if (result)
        {
            db_result_free(result);
        }
        if (empty)
        {
            line_upsert(ctx, "con0", ACCESS_LINE_TYPE_CON, 0, 0);
            for (uint32_t i = 0; i < ACCESS_VTY_COUNT; i++)
            {
                char key[16];
                snprintf(key, sizeof(key), "vty%u", i);
                line_upsert(ctx, key, ACCESS_LINE_TYPE_VTY, i, 0);
            }
            LOG_INFO("ACCESS: seeded default line rows (con0 + %d vty)", ACCESS_VTY_COUNT);
        }
    }
    return 0;
}

int access_db_restore(void)
{
    dev_ipc_context_t *ctx = access_ipc_ctx();

    /* 全局 telnet server 开关 */
    db_filter_builder_t fb;
    db_filter_init(&fb);
    db_filter_add_text(&fb, "skey", ACCESS_SETTING_TELNET);
    db_result_t *r = NULL;
    if (db_rpc_query(ctx, ACCESS_TABLE_SETTING, NULL, 0, &fb.filter, &r) == ERRCODE_SUCCESS && r && r->num_rows > 0)
    {
        access_set_telnet_server_enabled((int)db_row_get_int(r->rows[0], "sval", 0));
    }
    if (r)
    {
        db_result_free(r);
    }
    db_filter_clear(&fb);

    /* 各 vty 线 transport input */
    r = NULL;
    if (db_rpc_query(ctx, ACCESS_TABLE_LINE, NULL, 0, NULL, &r) == ERRCODE_SUCCESS && r)
    {
        for (uint32_t i = 0; i < r->num_rows; i++)
        {
            int line_type = (int)db_row_get_int(r->rows[i], "line_type", 0);
            uint32_t num = (uint32_t)db_row_get_int(r->rows[i], "line_num", 0);
            uint8_t transport = (uint8_t)db_row_get_int(r->rows[i], "transport", 0);
            if (line_type == ACCESS_LINE_TYPE_VTY)
            {
                access_vty_set_transport(num, num, transport);
            }
        }
    }
    if (r)
    {
        db_result_free(r);
    }
    return ERRCODE_SUCCESS;
}

int access_db_save_telnet_server(int enabled)
{
    dev_ipc_context_t *ctx = access_ipc_ctx();
    if (!db_rpc_is_available(ctx))
    {
        return ERRCODE_FAIL;
    }
    db_filter_builder_t fb;
    db_filter_init(&fb);
    db_filter_add_text(&fb, "skey", ACCESS_SETTING_TELNET);
    int delete_rc = db_rpc_delete(ctx, ACCESS_TABLE_SETTING, &fb.filter);
    db_filter_clear(&fb);
    /* db_rpc_delete returns the affected-row count; deleting one existing
     * setting is successful, while a negative value indicates an RPC/SQL
     * failure. */
    if (delete_rc < 0)
    {
        return ERRCODE_FAIL;
    }

    db_col_t cols[] = {DB_COL_TEXT("skey", ACCESS_SETTING_TELNET), DB_COL_INT("sval", enabled ? 1 : 0)};
    return db_rpc_insert_cols(ctx, ACCESS_TABLE_SETTING, cols, G_N_ELEMENTS(cols));
}

/** transport 位 → 字符串（BDR 本地用，避免跨文件依赖 access_line.c 的同名静态函数） */
static const char *bdr_transport_str(uint8_t bits)
{
    if ((bits & ACCESS_TRANSPORT_TELNET) && (bits & ACCESS_TRANSPORT_SSH))
    {
        return "all";
    }
    if (bits & ACCESS_TRANSPORT_TELNET)
    {
        return "telnet";
    }
    if (bits & ACCESS_TRANSPORT_SSH)
    {
        return "ssh";
    }
    return "none";
}

static int bdr_load_line_state(uint8_t vty_tr[ACCESS_VTY_COUNT], int *con_present)
{
    dev_ipc_context_t *ctx = access_ipc_ctx();
    if (!vty_tr || !con_present || !db_rpc_is_available(ctx))
    {
        return ERRCODE_FAIL;
    }

    memset(vty_tr, 0, ACCESS_VTY_COUNT);
    *con_present = 0;

    db_result_t *r = NULL;
    if (db_rpc_query(ctx, ACCESS_TABLE_LINE, NULL, 0, NULL, &r) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    if (r)
    {
        for (uint32_t i = 0; i < r->num_rows; i++)
        {
            int line_type = (int)db_row_get_int(r->rows[i], "line_type", 0);
            uint32_t num = (uint32_t)db_row_get_int(r->rows[i], "line_num", 0);
            uint8_t tr = (uint8_t)db_row_get_int(r->rows[i], "transport", 0);
            if (line_type == ACCESS_LINE_TYPE_CON)
            {
                *con_present = 1;
            }
            else if (line_type == ACCESS_LINE_TYPE_VTY && num < ACCESS_VTY_COUNT)
            {
                vty_tr[num] = tr;
            }
        }
        db_result_free(r);
    }

    return ERRCODE_SUCCESS;
}

void access_db_build_running_config(GString *out)
{
    dev_ipc_context_t *ctx = access_ipc_ctx();
    if (!out || !db_rpc_is_available(ctx))
    {
        return;
    }

    /* 全局 telnet server 开关（access_setting 表） */
    db_filter_builder_t fb;
    db_filter_init(&fb);
    db_filter_add_text(&fb, "skey", ACCESS_SETTING_TELNET);
    db_result_t *r = NULL;
    if (db_rpc_query(ctx, ACCESS_TABLE_SETTING, NULL, 0, &fb.filter, &r) == ERRCODE_SUCCESS && r && r->num_rows > 0 &&
        db_row_get_int(r->rows[0], "sval", 0))
    {
        g_string_append(out, "!\r\ntelnet server enable\r\n");
    }
    if (r)
    {
        db_result_free(r);
    }
    db_filter_clear(&fb);

    /* 各线（access_line 表）：con 线存在则输出 line console 0；vty 按 transport 分组 */
    uint8_t vty_tr[ACCESS_VTY_COUNT];
    int con_present = 0;
    if (bdr_load_line_state(vty_tr, &con_present) != ERRCODE_SUCCESS)
    {
        return;
    }

    if (con_present)
    {
        g_string_append(out, "!\r\nline console 0\r\n");
    }

    uint32_t i = 0;
    while (i < ACCESS_VTY_COUNT)
    {
        uint8_t t = vty_tr[i];
        if (t == 0) /* none 为默认，不输出 */
        {
            i++;
            continue;
        }
        uint32_t j = i;
        while (j + 1 < ACCESS_VTY_COUNT && vty_tr[j + 1] == t)
        {
            j++;
        }
        g_string_append(out, "!\r\n");
        g_string_append_printf(out, "line vty %u %u\r\n", i, j);
        g_string_append_printf(out, " transport input %s\r\n", bdr_transport_str(t));
        i = j + 1;
    }
}

void access_db_build_running_config_scoped(GString *out, const cli_show_scope_t *scope)
{
    if (!out || !scope || scope->mode != CLI_SHOW_SCOPE_MODE_THIS || !db_rpc_is_available(access_ipc_ctx()))
    {
        return;
    }

    if (strcmp(scope->view_name, CLI_VIEW_LINE_CONSOLE) == 0)
    {
        g_string_append(out, "!\r\nline console 0\r\n");
        return;
    }

    if (strcmp(scope->view_name, CLI_VIEW_LINE) != 0)
    {
        return;
    }

    uint32_t first = 0;
    uint32_t last = 0;
    if (cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, ACCESS_CTX_ID_LINE_FIRST, &first) != 0 ||
        cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, ACCESS_CTX_ID_LINE_LAST, &last) != 0)
    {
        return;
    }

    if (first >= ACCESS_VTY_COUNT)
    {
        return;
    }
    if (last >= ACCESS_VTY_COUNT)
    {
        last = ACCESS_VTY_COUNT - 1;
    }
    if (first > last)
    {
        return;
    }

    uint8_t vty_tr[ACCESS_VTY_COUNT];
    int con_present = 0;
    if (bdr_load_line_state(vty_tr, &con_present) != ERRCODE_SUCCESS)
    {
        return;
    }
    (void)con_present;

    gboolean emitted = FALSE;
    uint32_t i = first;
    while (i <= last)
    {
        uint8_t t = vty_tr[i];
        if (t == 0)
        {
            i++;
            continue;
        }

        uint32_t j = i;
        while (j + 1 <= last && vty_tr[j + 1] == t)
        {
            j++;
        }

        g_string_append(out, "!\r\n");
        g_string_append_printf(out, "line vty %u %u\r\n", i, j);
        g_string_append_printf(out, " transport input %s\r\n", bdr_transport_str(t));
        emitted = TRUE;
        i = j + 1;
    }

    if (!emitted)
    {
        g_string_append(out, "!\r\n");
        g_string_append_printf(out, "line vty %u %u\r\n", first, last);
    }
}

int access_db_save_vty_transport(uint32_t vty_num, uint8_t transport)
{
    dev_ipc_context_t *ctx = access_ipc_ctx();
    if (!db_rpc_is_available(ctx))
    {
        return ERRCODE_FAIL;
    }
    char key[16];
    snprintf(key, sizeof(key), "vty%u", vty_num);
    return line_upsert(ctx, key, ACCESS_LINE_TYPE_VTY, vty_num, transport);
}
