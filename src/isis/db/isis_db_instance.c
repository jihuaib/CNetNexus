/**
 * @file   isis_db_instance.c
 * @brief  ISIS instance 表：schema、CRUD、字段级 setter、启动恢复
 * @author jhb
 * @date   2026/04/26
 */

#include <string.h>

#include "errcode.h"
#include "isis_db_internal.h"
#include "log.h"
#include "vrf.h"

#define ISIS_VRF_TABLE_INSTANCE "vrf_instance"

static const db_column_def_t ISIS_INSTANCE_COLS[] = {
    {"tag", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},    {"net", DB_TYPE_TEXT, DB_COL_NOT_NULL, ""},
    {"is_type", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "3"},    {"admin_up", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"af_ipv4", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},    {"af_ipv6", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"cost_style", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"}, {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"vrf_name", DB_TYPE_TEXT, DB_COL_NOT_NULL, "public"}, {"srv6_locator", DB_TYPE_TEXT, DB_COL_NOT_NULL, ""},
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

int isis_db_set_instance(uint32_t tag, uint32_t vrf_id, const char *vrf_name)
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
        db_col_t update_cols[] = {
            DB_COL_INT("vrf_id", vrf_id),
            DB_COL_TEXT("vrf_name", vrf_name),
        };
        isis_db_instance_pk(&pk, tag);
        int rows = db_rpc_update_cols(ctx, ISIS_TABLE_INSTANCE, &pk.filter, update_cols, G_N_ELEMENTS(update_cols));
        db_filter_clear(&pk);
        return rows >= 0 ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    db_col_t cols[] = {
        DB_COL_INT("tag", tag),
        DB_COL_TEXT("net", ""),
        DB_COL_INT("is_type", ISIS_IS_TYPE_LEVEL_1_2),
        DB_COL_INT("admin_up", 1),
        DB_COL_INT("af_ipv4", 1),
        DB_COL_INT("af_ipv6", 1),
        DB_COL_INT("cost_style", ISIS_DEFAULT_COST_STYLE),
        DB_COL_INT("vrf_id", vrf_id),
        DB_COL_TEXT("vrf_name", vrf_name),
        DB_COL_TEXT("srv6_locator", ""),
    };
    return db_rpc_insert_cols(ctx, ISIS_TABLE_INSTANCE, cols, G_N_ELEMENTS(cols));
}

int isis_db_get_instance_vrf(uint32_t tag, uint32_t *vrf_id_out, char *vrf_name_out, size_t vrf_name_out_size)
{
    if (tag == 0u)
    {
        return ERRCODE_FAIL;
    }
    db_filter_builder_t pk;
    isis_db_instance_pk(&pk, tag);
    db_result_t *result = NULL;
    int rc = db_rpc_query(isis_local_ipc_ctx(), ISIS_TABLE_INSTANCE, NULL, 0, &pk.filter, &result);
    db_filter_clear(&pk);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }
    int64_t vrf_id = db_row_get_int(result->rows[0], "vrf_id", VRF_PUBLIC_VRF_ID);
    if (vrf_id_out)
    {
        *vrf_id_out = (vrf_id >= 0 && (uint64_t)vrf_id <= UINT32_MAX) ? (uint32_t)vrf_id : VRF_PUBLIC_VRF_ID;
    }
    if (vrf_name_out && vrf_name_out_size > 0u)
    {
        g_strlcpy(vrf_name_out, db_row_get_text(result->rows[0], "vrf_name", VRF_PUBLIC_VRF_NAME), vrf_name_out_size);
    }
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int isis_db_resolve_vrf(const char *vrf_name, uint32_t *vrf_id_out)
{
    const char *name = (vrf_name && vrf_name[0] != '\0') ? vrf_name : VRF_PUBLIC_VRF_NAME;
    if (!vrf_id_out)
    {
        return ERRCODE_FAIL;
    }
    if (strcmp(name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        *vrf_id_out = VRF_PUBLIC_VRF_ID;
        return ERRCODE_SUCCESS;
    }
    db_filter_builder_t filter;
    db_filter_init(&filter);
    db_filter_add_text(&filter, "name", name);
    const char *fields[] = {"vrf_id"};
    db_result_t *result = NULL;
    int rc = db_rpc_query(isis_local_ipc_ctx(), ISIS_VRF_TABLE_INSTANCE, fields, G_N_ELEMENTS(fields), &filter.filter,
                          &result);
    db_filter_clear(&filter);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }
    int64_t vrf_id = db_row_get_int(result->rows[0], "vrf_id", -1);
    db_result_free(result);
    if (vrf_id <= 0 || (uint64_t)vrf_id > UINT32_MAX)
    {
        return ERRCODE_FAIL;
    }
    *vrf_id_out = (uint32_t)vrf_id;
    return ERRCODE_SUCCESS;
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

    int inst_rc = db_rpc_delete(ctx, ISIS_TABLE_INSTANCE, &inst_pk.filter);
    int if_rc = db_rpc_delete(ctx, ISIS_TABLE_INTERFACE, &if_pk.filter);

    db_filter_clear(&inst_pk);
    db_filter_clear(&if_pk);
    return (inst_rc >= 0 && if_rc >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
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

int isis_db_set_cost_style(uint32_t tag, uint8_t cost_style)
{
    if (tag == 0u || (cost_style != ISIS_COST_STYLE_NARROW && cost_style != ISIS_COST_STYLE_WIDE))
    {
        return ERRCODE_FAIL;
    }
    return isis_db_update_instance_field_u32(tag, "cost_style", (uint32_t)cost_style);
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

int isis_db_set_srv6_locator(uint32_t tag, const char *locator)
{
    if (tag == 0u || !locator || strlen(locator) >= SRV6_LOCATOR_NAME_MAX)
    {
        return ERRCODE_FAIL;
    }
    return isis_db_update_instance_field_text(tag, "srv6_locator", locator);
}

int isis_db_get_srv6_locator(uint32_t tag, char *locator_out, size_t locator_out_size)
{
    if (tag == 0u || !locator_out || locator_out_size == 0u)
    {
        return ERRCODE_FAIL;
    }
    locator_out[0] = '\0';
    db_filter_builder_t pk;
    isis_db_instance_pk(&pk, tag);
    const char *fields[] = {"srv6_locator"};
    db_result_t *result = NULL;
    int rc = db_rpc_query(isis_local_ipc_ctx(), ISIS_TABLE_INSTANCE, fields, G_N_ELEMENTS(fields), &pk.filter, &result);
    db_filter_clear(&pk);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }
    g_strlcpy(locator_out, db_row_get_text(result->rows[0], "srv6_locator", ""), locator_out_size);
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

static gboolean isis_db_restore_apply_ok(isis_apply_cmd_t *apply, const char *stage, uint32_t tag)
{
    if (!apply || !stage)
    {
        return FALSE;
    }

    if (isis_worker_dispatch_apply(apply) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS restore: failed to dispatch %s for process %u", stage, tag);
        return FALSE;
    }
    if (apply->rc == ISIS_APPLY_RC_OK || apply->rc == ISIS_APPLY_RC_NOOP)
    {
        return TRUE;
    }

    LOG_ERROR("ISIS restore: %s failed for process %u: %s", stage, tag,
              apply->errmsg[0] != '\0' ? apply->errmsg : "unknown apply error");
    return FALSE;
}

int isis_db_restore_instances(void)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS restore: failed to query instances");
        return ERRCODE_FAIL;
    }
    if (!result)
    {
        return ERRCODE_SUCCESS;
    }

    int restore_rc = ERRCODE_SUCCESS;

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
        apply.u.instance_set.vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", VRF_PUBLIC_VRF_ID);
        const char *vrf_name = db_row_get_text(row, "vrf_name", VRF_PUBLIC_VRF_NAME);
        g_strlcpy(apply.u.instance_set.vrf_name, (vrf_name && vrf_name[0] != '\0') ? vrf_name : VRF_PUBLIC_VRF_NAME,
                  sizeof(apply.u.instance_set.vrf_name));
        const char *net = db_row_get_text(row, "net", "");
        g_strlcpy(apply.u.instance_set.net, net ? net : "", sizeof(apply.u.instance_set.net));
        if (!isis_db_restore_apply_ok(&apply, "instance", tag))
        {
            restore_rc = ERRCODE_FAIL;
            continue;
        }

        uint8_t cost_style = (uint8_t)db_row_get_int(row, "cost_style", ISIS_DEFAULT_COST_STYLE);
        if (cost_style != ISIS_COST_STYLE_NARROW && cost_style != ISIS_COST_STYLE_WIDE)
        {
            cost_style = ISIS_DEFAULT_COST_STYLE;
        }
        isis_apply_cmd_t cs_apply;
        memset(&cs_apply, 0, sizeof(cs_apply));
        cs_apply.op = ISIS_APPLY_OP_COST_STYLE_SET;
        cs_apply.u.cost_style_set.tag = tag;
        cs_apply.u.cost_style_set.cost_style = cost_style;
        (void)isis_worker_dispatch_apply(&cs_apply);

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
        if (!isis_db_restore_apply_ok(&af_apply, "IPv6 AF", tag))
        {
            restore_rc = ERRCODE_FAIL;
            continue;
        }

        const char *srv6_locator = db_row_get_text(row, "srv6_locator", "");
        if (srv6_locator && srv6_locator[0] != '\0')
        {
            isis_apply_cmd_t srv6_apply;
            memset(&srv6_apply, 0, sizeof(srv6_apply));
            srv6_apply.op = ISIS_APPLY_OP_SRV6_LOCATOR_SET;
            srv6_apply.u.srv6_locator_set.tag = tag;
            g_strlcpy(srv6_apply.u.srv6_locator_set.locator, srv6_locator,
                      sizeof(srv6_apply.u.srv6_locator_set.locator));
            if (!isis_db_restore_apply_ok(&srv6_apply, "SRv6 locator", tag))
            {
                restore_rc = ERRCODE_FAIL;
            }
        }
    }

    db_result_free(result);
    return restore_rc;
}
