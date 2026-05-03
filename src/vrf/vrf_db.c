/**
 * @file   vrf_db.c
 * @brief  VRF 持久化实现（封装 db_rpc）
 * @author jhb
 * @date   2026/05/02
 */
#include "vrf_db.h"

#include <string.h>

#include "db.h"
#include "errcode.h"
#include "log.h"
#include "vrf_main.h"
#include "work/vrf_os.h"
#include "work/vrf_pub.h"
#include "work/vrf_table.h"
#include "work/vrf_worker.h"

// ============================================================================
// 表定义
// ============================================================================

static const db_column_def_t VRF_INSTANCE_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
    {"name", DB_TYPE_TEXT, DB_COL_NOT_NULL | DB_COL_UNIQUE, NULL},
    {"l3vrf_table_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
};
static const db_table_def_t VRF_INSTANCE_TBL = {
    .table_name = VRF_TABLE_INSTANCE,
    .cols = VRF_INSTANCE_COLS,
    .num_cols = G_N_ELEMENTS(VRF_INSTANCE_COLS),
};

static const db_column_def_t VRF_AF_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"safi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"has_rd", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"rd", DB_TYPE_TEXT, DB_COL_NONE, NULL},
};
static const db_table_def_t VRF_AF_TBL = {
    .table_name = VRF_TABLE_AF,
    .cols = VRF_AF_COLS,
    .num_cols = G_N_ELEMENTS(VRF_AF_COLS),
};

static const db_column_def_t VRF_RT_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL}, {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"safi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},   {"direction", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"rt", DB_TYPE_BLOB, DB_COL_NOT_NULL, NULL},
};
static const db_table_def_t VRF_RT_TBL = {
    .table_name = VRF_TABLE_RT,
    .cols = VRF_RT_COLS,
    .num_cols = G_N_ELEMENTS(VRF_RT_COLS),
};

// ============================================================================
// 16 字符 hex 编解码（与 vrf_bdr 保持一致；RD/RT 都用同一种文本编码）
// ============================================================================

static void bytes_to_hex16(const uint8_t bytes[8], char out[17])
{
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
    {
        out[i * 2] = H[(bytes[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[bytes[i] & 0xF];
    }
    out[16] = '\0';
}

static int parse_hex16_bytes(const char *s, uint8_t out[8])
{
    if (!s || strlen(s) != 16)
    {
        return -1;
    }
    for (int i = 0; i < 8; i++)
    {
        unsigned int byte = 0;
        if (sscanf(s + i * 2, "%2x", &byte) != 1)
        {
            return -1;
        }
        out[i] = (uint8_t)byte;
    }
    return 0;
}

// ============================================================================
// 公共 API
// ============================================================================

int vrf_db_init(void)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    if (db_rpc_create_table_from_def(ctx, &VRF_INSTANCE_TBL) != ERRCODE_SUCCESS ||
        db_rpc_create_table_from_def(ctx, &VRF_AF_TBL) != ERRCODE_SUCCESS ||
        db_rpc_create_table_from_def(ctx, &VRF_RT_TBL) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("VRF: failed to create persistence tables");
        return -1;
    }
    return 0;
}

int vrf_db_insert_vrf(uint32_t vrf_id, const char *name, uint32_t l3vrf_table_id)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    db_col_t cols[] = {
        DB_COL_INT("vrf_id", (int64_t)vrf_id),
        DB_COL_TEXT("name", name),
        DB_COL_INT("l3vrf_table_id", (int64_t)l3vrf_table_id),
    };
    if (db_rpc_insert_cols(ctx, VRF_TABLE_INSTANCE, cols, G_N_ELEMENTS(cols)) != ERRCODE_SUCCESS)
    {
        return -1;
    }
    return 0;
}

static int delete_by_vrf_id(const char *table, uint32_t vrf_id)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    db_filter_builder_t b;
    db_filter_init(&b);
    db_filter_add_int(&b, "vrf_id", (int64_t)vrf_id);
    int n = db_rpc_delete(ctx, table, &b.filter);
    db_filter_clear(&b);
    return n;
}

int vrf_db_delete_vrf(uint32_t vrf_id)
{
    (void)delete_by_vrf_id(VRF_TABLE_RT, vrf_id);
    (void)delete_by_vrf_id(VRF_TABLE_AF, vrf_id);
    return delete_by_vrf_id(VRF_TABLE_INSTANCE, vrf_id);
}

static void af_filter_init(db_filter_builder_t *b, uint32_t vrf_id, uint16_t afi, uint8_t safi)
{
    db_filter_init(b);
    db_filter_add_int(b, "vrf_id", (int64_t)vrf_id);
    db_filter_add_int(b, "afi", (int64_t)afi);
    db_filter_add_int(b, "safi", (int64_t)safi);
}

int vrf_db_set_af_rd(uint32_t vrf_id, uint16_t afi, uint8_t safi, const vrf_rd_t *rd)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();

    char hex[17] = {0};
    if (rd)
    {
        bytes_to_hex16(rd->bytes, hex);
    }

    /* 先尝试 update，未命中则 insert */
    db_filter_builder_t b;
    af_filter_init(&b, vrf_id, afi, safi);

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "has_rd", rd ? 1 : 0);
    if (rd)
    {
        db_record_set_text(rec, "rd", hex);
    }
    else
    {
        db_record_set_text(rec, "rd", "");
    }

    int updated = db_rpc_update_record(ctx, VRF_TABLE_AF, rec, &b.filter);
    db_record_free(rec);

    if (updated <= 0)
    {
        db_record_t *ins = db_record_new();
        db_record_set_int(ins, "vrf_id", (int64_t)vrf_id);
        db_record_set_int(ins, "afi", (int64_t)afi);
        db_record_set_int(ins, "safi", (int64_t)safi);
        db_record_set_int(ins, "has_rd", rd ? 1 : 0);
        if (rd)
        {
            db_record_set_text(ins, "rd", hex);
        }
        int ret = db_rpc_insert_record(ctx, VRF_TABLE_AF, ins);
        db_record_free(ins);
        if (ret != ERRCODE_SUCCESS)
        {
            db_filter_clear(&b);
            return -1;
        }
    }
    db_filter_clear(&b);
    return 0;
}

int vrf_db_delete_af(uint32_t vrf_id, uint16_t afi, uint8_t safi)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    db_filter_builder_t b;
    af_filter_init(&b, vrf_id, afi, safi);
    (void)db_rpc_delete(ctx, VRF_TABLE_RT, &b.filter);
    int n = db_rpc_delete(ctx, VRF_TABLE_AF, &b.filter);
    db_filter_clear(&b);
    return n;
}

int vrf_db_modify_rt(uint32_t vrf_id, uint16_t afi, uint8_t safi, int direction, int add, const vrf_rt_t *rt)
{
    if (!rt)
    {
        return -1;
    }
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();

    char hex[17];
    bytes_to_hex16(rt->bytes, hex);

    if (add)
    {
        db_col_t cols[] = {
            DB_COL_INT("vrf_id", (int64_t)vrf_id),
            DB_COL_INT("afi", (int64_t)afi),
            DB_COL_INT("safi", (int64_t)safi),
            DB_COL_INT("direction", (int64_t)direction),
            DB_COL_TEXT("rt", hex),
        };
        return (db_rpc_insert_cols(ctx, VRF_TABLE_RT, cols, G_N_ELEMENTS(cols)) == ERRCODE_SUCCESS) ? 0 : -1;
    }

    db_filter_builder_t b;
    db_filter_init(&b);
    db_filter_add_int(&b, "vrf_id", (int64_t)vrf_id);
    db_filter_add_int(&b, "afi", (int64_t)afi);
    db_filter_add_int(&b, "safi", (int64_t)safi);
    db_filter_add_int(&b, "direction", (int64_t)direction);
    db_filter_add_text(&b, "rt", hex);
    int n = db_rpc_delete(ctx, VRF_TABLE_RT, &b.filter);
    db_filter_clear(&b);
    return (n >= 0) ? 0 : -1;
}

// ============================================================================
// 快照装载（worker 线程调用）
// ============================================================================

int vrf_db_load_snapshot(void)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    vrf_table_t *t = vrf_worker_table();
    if (!ctx || !t)
    {
        return -1;
    }

    /* VRF 实例 */
    db_result_t *res = NULL;
    if (db_rpc_query(ctx, VRF_TABLE_INSTANCE, NULL, 0, NULL, &res) != ERRCODE_SUCCESS)
    {
        return -1;
    }
    if (res)
    {
        for (uint32_t i = 0; i < res->num_rows; i++)
        {
            db_row_t *row = res->rows[i];
            uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", 0);
            const char *name = db_row_get_text(row, "name", NULL);
            uint32_t table_id = (uint32_t)db_row_get_int(row, "l3vrf_table_id", 0);
            if (!name)
            {
                continue;
            }
            vrf_entry_t *e = vrf_table_create_with_id(t, vrf_id, name, table_id);
            if (!e)
            {
                continue;
            }
            if (vrf_id != VRF_PUBLIC_VRF_ID)
            {
                if (vrf_os_install(e->name, e->l3vrf_table_id) == 0)
                {
                    e->os_state = VRF_OS_STATE_UP;
                }
                else
                {
                    e->os_state = VRF_OS_STATE_DOWN;
                }
            }
        }
        db_result_free(res);
        res = NULL;
    }

    /* AF + RD */
    if (db_rpc_query(ctx, VRF_TABLE_AF, NULL, 0, NULL, &res) != ERRCODE_SUCCESS)
    {
        return -1;
    }
    if (res)
    {
        for (uint32_t i = 0; i < res->num_rows; i++)
        {
            db_row_t *row = res->rows[i];
            uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", 0);
            uint16_t afi = (uint16_t)db_row_get_int(row, "afi", 0);
            uint8_t safi = (uint8_t)db_row_get_int(row, "safi", 0);
            int has_rd = (int)db_row_get_int(row, "has_rd", 0);
            const char *rd_hex = db_row_get_text(row, "rd", NULL);

            vrf_entry_t *e = vrf_table_find_by_id(t, vrf_id);
            if (!e)
            {
                continue;
            }
            vrf_af_state_t *af = vrf_af_get_or_create(e, afi, safi);
            if (af && has_rd && rd_hex)
            {
                vrf_rd_t rd;
                if (parse_hex16_bytes(rd_hex, rd.bytes) == 0)
                {
                    af->has_rd = 1;
                    af->rd = rd;
                }
            }
        }
        db_result_free(res);
        res = NULL;
    }

    /* RT */
    if (db_rpc_query(ctx, VRF_TABLE_RT, NULL, 0, NULL, &res) != ERRCODE_SUCCESS)
    {
        return -1;
    }
    if (res)
    {
        for (uint32_t i = 0; i < res->num_rows; i++)
        {
            db_row_t *row = res->rows[i];
            uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", 0);
            uint16_t afi = (uint16_t)db_row_get_int(row, "afi", 0);
            uint8_t safi = (uint8_t)db_row_get_int(row, "safi", 0);
            int direction = (int)db_row_get_int(row, "direction", 0);
            const char *hex = db_row_get_text(row, "rt", NULL);

            vrf_entry_t *e = vrf_table_find_by_id(t, vrf_id);
            if (!e || !hex)
            {
                continue;
            }
            vrf_af_state_t *af = vrf_af_get_or_create(e, afi, safi);
            if (!af)
            {
                continue;
            }
            vrf_rt_t rt;
            if (parse_hex16_bytes(hex, rt.bytes) != 0)
            {
                continue;
            }
            (void)vrf_af_modify_rt(af, direction, 1, &rt);
        }
        db_result_free(res);
    }
    return 0;
}
