/**
 * @file   isis_db_instance.c
 * @brief  ISIS instance 表：schema、CRUD、字段级 setter、启动恢复
 * @author jhb
 * @date   2026/04/26
 */

#include <string.h>

#include "errcode.h"
#include "isis_db_internal.h"

static const db_column_def_t ISIS_INSTANCE_COLS[] = {
    {"tag", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL}, {"net", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"is_type", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "3"}, {"admin_up", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"af_ipv4", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"}, {"af_ipv6", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
};

const db_table_def_t ISIS_INSTANCE_TABLE = {
    .table_name = ISIS_TABLE_INSTANCE,
    .cols = ISIS_INSTANCE_COLS,
    .num_cols = G_N_ELEMENTS(ISIS_INSTANCE_COLS),
};

static int isis_db_update_instance_field_u32(uint32_t tag, const char *field, uint32_t value)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx || tag == 0u || !field)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    isis_db_instance_pk(&pk, tag);

    db_col_t cols[] = {
        DB_COL_INT(field, value),
    };
    int rows = db_rpc_update_cols(ctx, ISIS_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);
    return (rows > 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int isis_db_update_instance_field_text(uint32_t tag, const char *field, const char *text)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx || tag == 0u || !field)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    isis_db_instance_pk(&pk, tag);

    db_col_t cols[] = {
        DB_COL_TEXT(field, text ? text : ""),
    };
    int rows = db_rpc_update_cols(ctx, ISIS_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);
    return (rows > 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int isis_db_set_instance(uint32_t tag)
{
    if (tag == 0u)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    isis_db_instance_pk(&pk, tag);

    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, ISIS_TABLE_INSTANCE, &pk.filter, &exists);
    db_filter_clear(&pk);
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (exists)
    {
        return ERRCODE_SUCCESS;
    }

    db_col_t cols[] = {
        DB_COL_INT("tag", tag),    DB_COL_TEXT("net", ""),   DB_COL_INT("is_type", ISIS_IS_TYPE_LEVEL_1_2),
        DB_COL_INT("admin_up", 1), DB_COL_INT("af_ipv4", 1), DB_COL_INT("af_ipv6", 1),
    };
    return db_rpc_insert_cols(ctx, ISIS_TABLE_INSTANCE, cols, G_N_ELEMENTS(cols));
}

int isis_db_del_instance(uint32_t tag)
{
    if (tag == 0u)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t inst_pk;
    db_filter_builder_t if_pk;
    isis_db_instance_pk(&inst_pk, tag);
    isis_db_interface_tag_pk(&if_pk, tag);

    (void)db_rpc_delete(ctx, ISIS_TABLE_INSTANCE, &inst_pk.filter);
    (void)db_rpc_delete(ctx, ISIS_TABLE_INTERFACE, &if_pk.filter);

    db_filter_clear(&inst_pk);
    db_filter_clear(&if_pk);
    return ERRCODE_SUCCESS;
}

int isis_db_set_net(uint32_t tag, const char *net)
{
    if (tag == 0u || !net)
    {
        return ERRCODE_FAIL;
    }
    return isis_db_update_instance_field_text(tag, "net", net);
}

int isis_db_set_is_type(uint32_t tag, uint8_t is_type)
{
    if (tag == 0u ||
        (is_type != ISIS_IS_TYPE_LEVEL_1 && is_type != ISIS_IS_TYPE_LEVEL_2 && is_type != ISIS_IS_TYPE_LEVEL_1_2))
    {
        return ERRCODE_FAIL;
    }
    return isis_db_update_instance_field_u32(tag, "is_type", (uint32_t)is_type);
}

int isis_db_set_af(uint32_t tag, uint16_t afi, int enabled)
{
    if (tag == 0u)
    {
        return ERRCODE_FAIL;
    }

    if (afi == ISIS_AFI_IPV4)
    {
        return isis_db_update_instance_field_u32(tag, "af_ipv4", enabled ? 1u : 0u);
    }
    if (afi == ISIS_AFI_IPV6)
    {
        return isis_db_update_instance_field_u32(tag, "af_ipv6", enabled ? 1u : 0u);
    }
    return ERRCODE_FAIL;
}

int isis_db_is_af_enabled(uint32_t tag, uint16_t afi, int *enabled_out)
{
    if (tag == 0u || !enabled_out)
    {
        return ERRCODE_FAIL;
    }

    *enabled_out = 0;
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    isis_db_instance_pk(&pk, tag);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, &pk.filter, &result);
    db_filter_clear(&pk);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    db_row_t *row = result->rows[0];
    if (afi == ISIS_AFI_IPV4)
    {
        *enabled_out = db_row_get_int(row, "af_ipv4", 1) ? 1 : 0;
    }
    else if (afi == ISIS_AFI_IPV6)
    {
        *enabled_out = db_row_get_int(row, "af_ipv6", 1) ? 1 : 0;
    }
    else
    {
        db_result_free(result);
        return ERRCODE_FAIL;
    }

    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int isis_db_get_default_tag(uint32_t *tag_out)
{
    if (!tag_out)
    {
        return ERRCODE_FAIL;
    }

    *tag_out = 0u;
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result ||
        result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    uint32_t best = 0u;
    for (uint32_t i = 0u; i < result->num_rows; ++i)
    {
        db_row_t *row = result->rows[i];
        uint32_t tag = (uint32_t)db_row_get_int(row, "tag", 0);
        if (tag == 0u)
        {
            continue;
        }
        if (best == 0u || tag < best)
        {
            best = tag;
        }
    }
    db_result_free(result);

    if (best == 0u)
    {
        return ERRCODE_FAIL;
    }

    *tag_out = best;
    return ERRCODE_SUCCESS;
}

void isis_db_restore_instances(void)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0u; i < result->num_rows; ++i)
    {
        db_row_t *row = result->rows[i];
        uint32_t tag = (uint32_t)db_row_get_int(row, "tag", 0);
        if (tag == 0u)
        {
            continue;
        }

        isis_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = ISIS_APPLY_OP_INSTANCE_SET;
        apply.u.instance_set.tag = tag;
        apply.u.instance_set.is_type = (uint8_t)db_row_get_int(row, "is_type", ISIS_IS_TYPE_LEVEL_1_2);
        apply.u.instance_set.admin_up = (uint8_t)(db_row_get_int(row, "admin_up", 1) ? 1 : 0);
        const char *net = db_row_get_text(row, "net", "");
        g_strlcpy(apply.u.instance_set.net, net ? net : "", sizeof(apply.u.instance_set.net));
        (void)isis_worker_dispatch_apply(&apply);

        int64_t af4 = db_row_get_int(row, "af_ipv4", 1);
        int64_t af6 = db_row_get_int(row, "af_ipv6", 1);

        isis_apply_cmd_t af_apply;
        memset(&af_apply, 0, sizeof(af_apply));
        af_apply.op = (af4 != 0) ? ISIS_APPLY_OP_AF_SET : ISIS_APPLY_OP_AF_DEL;
        af_apply.u.af_set.tag = tag;
        af_apply.u.af_set.afi = ISIS_AFI_IPV4;
        (void)isis_worker_dispatch_apply(&af_apply);

        memset(&af_apply, 0, sizeof(af_apply));
        af_apply.op = (af6 != 0) ? ISIS_APPLY_OP_AF_SET : ISIS_APPLY_OP_AF_DEL;
        af_apply.u.af_set.tag = tag;
        af_apply.u.af_set.afi = ISIS_AFI_IPV6;
        (void)isis_worker_dispatch_apply(&af_apply);
    }

    db_result_free(result);
}
