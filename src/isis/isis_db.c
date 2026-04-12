/**
 * @file   isis_db.c
 * @brief  ISIS 模块数据库操作实现
 * @author jhb
 * @date   2026/04/11
 */
#include "isis_db.h"

#include <string.h>

#include "db.h"
#include "errcode.h"
#include "isis.h"
#include "isis_main.h"
#include "log.h"

static const db_column_def_t ISIS_INSTANCE_COLS[] = {
    {"tag", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL}, {"net", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"is_type", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "3"}, {"admin_up", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"af_ipv4", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"}, {"af_ipv6", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
};

static const db_table_def_t ISIS_INSTANCE_TABLE = {
    .table_name = ISIS_TABLE_INSTANCE,
    .cols = ISIS_INSTANCE_COLS,
    .num_cols = G_N_ELEMENTS(ISIS_INSTANCE_COLS),
};

static const db_column_def_t ISIS_IF_COLS[] = {
    {"id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY | DB_COL_AUTOINCREMENT, NULL},
    {"tag", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"ifname", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"enabled", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"metric", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "10"},
    {"hello_interval", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "10"},
    {"hold_multiplier", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "3"},
    {"passive", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
};

static const db_table_def_t ISIS_IF_TABLE = {
    .table_name = ISIS_TABLE_INTERFACE,
    .cols = ISIS_IF_COLS,
    .num_cols = G_N_ELEMENTS(ISIS_IF_COLS),
};

typedef struct isis_restore_if_item
{
    uint32_t tag;
    isis_if_cfg_t cfg;
} isis_restore_if_item_t;

static void isis_if_af_cfg_defaults(isis_if_af_cfg_t *cfg)
{
    if (!cfg)
    {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 0u;
    cfg->metric = ISIS_DEFAULT_IF_METRIC;
    cfg->hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
    cfg->hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
    cfg->passive = ISIS_DEFAULT_IF_PASSIVE;
}

static void isis_if_af_cfg_sanitize(isis_if_af_cfg_t *cfg)
{
    if (!cfg)
    {
        return;
    }
    cfg->enabled = cfg->enabled ? 1u : 0u;
    if (cfg->metric == 0u)
    {
        cfg->metric = ISIS_DEFAULT_IF_METRIC;
    }
    if (cfg->metric > ISIS_MAX_IF_METRIC)
    {
        cfg->metric = ISIS_MAX_IF_METRIC;
    }
    if (cfg->hello_interval == 0u)
    {
        cfg->hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
    }
    if (cfg->hold_multiplier == 0u)
    {
        cfg->hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
    }
    if (cfg->hold_multiplier > ISIS_MAX_HOLD_MULTIPLIER)
    {
        cfg->hold_multiplier = ISIS_MAX_HOLD_MULTIPLIER;
    }
    cfg->passive = cfg->passive ? 1u : 0u;
}

static void isis_if_cfg_defaults(isis_if_cfg_t *cfg, const char *ifname)
{
    if (!cfg)
    {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    if (ifname)
    {
        g_strlcpy(cfg->ifname, ifname, sizeof(cfg->ifname));
    }
    isis_if_af_cfg_defaults(&cfg->v4);
    isis_if_af_cfg_defaults(&cfg->v6);
    cfg->last_hello_tx_msec = 0u;
}

static void isis_if_apply_row_to_af(db_row_t *row, isis_if_af_cfg_t *af_cfg)
{
    if (!row || !af_cfg)
    {
        return;
    }

    af_cfg->enabled = db_row_get_int(row, "enabled", 1) ? 1u : 0u;

    int64_t metric = db_row_get_int(row, "metric", ISIS_DEFAULT_IF_METRIC);
    int64_t hello = db_row_get_int(row, "hello_interval", ISIS_DEFAULT_HELLO_INTERVAL);
    int64_t hold = db_row_get_int(row, "hold_multiplier", ISIS_DEFAULT_HOLD_MULTIPLIER);
    int64_t passive = db_row_get_int(row, "passive", ISIS_DEFAULT_IF_PASSIVE);

    if (metric > 0 && metric <= ISIS_MAX_IF_METRIC)
    {
        af_cfg->metric = (uint32_t)metric;
    }
    if (hello > 0 && hello <= ISIS_MAX_HELLO_INTERVAL)
    {
        af_cfg->hello_interval = (uint16_t)hello;
    }
    if (hold > 0 && hold <= ISIS_MAX_HOLD_MULTIPLIER)
    {
        af_cfg->hold_multiplier = (uint8_t)hold;
    }
    af_cfg->passive = passive ? 1u : 0u;
    isis_if_af_cfg_sanitize(af_cfg);
}

static void isis_restore_if_item_free(gpointer data)
{
    g_free(data);
}

static int db_update_instance_field_u32(uint32_t tag, const char *field, uint32_t value)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    db_condition_t cond = {
        .field_name = "tag",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)tag),
    };
    db_filter_t filter = {
        .conditions = &cond,
        .num_conditions = 1,
    };

    db_record_t *rec = db_record_new();
    if (!rec)
    {
        db_value_free(&cond.value);
        return ERRCODE_FAIL;
    }
    db_record_set_int(rec, field, (int64_t)value);

    int rows = db_rpc_update_record(ctx, ISIS_TABLE_INSTANCE, rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);

    return (rows >= 1) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int db_update_instance_field_text(uint32_t tag, const char *field, const char *text)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    db_condition_t cond = {
        .field_name = "tag",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)tag),
    };
    db_filter_t filter = {
        .conditions = &cond,
        .num_conditions = 1,
    };

    db_record_t *rec = db_record_new();
    if (!rec)
    {
        db_value_free(&cond.value);
        return ERRCODE_FAIL;
    }
    db_record_set_text(rec, field, text ? text : "");

    int rows = db_rpc_update_record(ctx, ISIS_TABLE_INSTANCE, rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);

    return (rows >= 1) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int isis_db_init(void)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    if (db_rpc_create_table_from_def(ctx, &ISIS_INSTANCE_TABLE) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: create table %s failed", ISIS_TABLE_INSTANCE);
        return ERRCODE_FAIL;
    }

    if (db_rpc_create_table_from_def(ctx, &ISIS_IF_TABLE) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: create table %s failed", ISIS_TABLE_INTERFACE);
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int isis_db_set_instance(uint32_t tag)
{
    if (tag == 0u)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    db_condition_t cond = {
        .field_name = "tag",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)tag),
    };
    db_filter_t filter = {
        .conditions = &cond,
        .num_conditions = 1,
    };

    gboolean exists = FALSE;
    int exists_rc = db_rpc_exists(ctx, ISIS_TABLE_INSTANCE, &filter, &exists);
    db_value_free(&cond.value);
    if (exists_rc != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (exists)
    {
        return ERRCODE_SUCCESS;
    }

    db_record_t *rec = db_record_new();
    if (!rec)
    {
        return ERRCODE_FAIL;
    }
    db_record_set_int(rec, "tag", (int64_t)tag);
    db_record_set_text(rec, "net", "");
    db_record_set_int(rec, "is_type", ISIS_IS_TYPE_LEVEL_1_2);
    db_record_set_int(rec, "admin_up", 1);
    db_record_set_int(rec, "af_ipv4", 1);
    db_record_set_int(rec, "af_ipv6", 1);

    int rc = db_rpc_insert_record(ctx, ISIS_TABLE_INSTANCE, rec);
    db_record_free(rec);
    return rc;
}

int isis_db_del_instance(uint32_t tag)
{
    if (tag == 0u)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();

    db_condition_t inst_cond = {
        .field_name = "tag",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)tag),
    };
    db_filter_t inst_filter = {
        .conditions = &inst_cond,
        .num_conditions = 1,
    };
    (void)db_rpc_delete(ctx, ISIS_TABLE_INSTANCE, &inst_filter);
    db_value_free(&inst_cond.value);

    db_condition_t if_cond = {
        .field_name = "tag",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)tag),
    };
    db_filter_t if_filter = {
        .conditions = &if_cond,
        .num_conditions = 1,
    };
    (void)db_rpc_delete(ctx, ISIS_TABLE_INTERFACE, &if_filter);
    db_value_free(&if_cond.value);

    return ERRCODE_SUCCESS;
}

int isis_db_set_net(uint32_t tag, const char *net)
{
    if (tag == 0u || !net)
    {
        return ERRCODE_FAIL;
    }
    return db_update_instance_field_text(tag, "net", net);
}

int isis_db_set_is_type(uint32_t tag, uint8_t is_type)
{
    if (tag == 0u ||
        (is_type != ISIS_IS_TYPE_LEVEL_1 && is_type != ISIS_IS_TYPE_LEVEL_2 && is_type != ISIS_IS_TYPE_LEVEL_1_2))
    {
        return ERRCODE_FAIL;
    }
    return db_update_instance_field_u32(tag, "is_type", (uint32_t)is_type);
}

int isis_db_set_af(uint32_t tag, uint16_t afi, int enabled)
{
    if (tag == 0u)
    {
        return ERRCODE_FAIL;
    }

    if (afi == ISIS_AFI_IPV4)
    {
        return db_update_instance_field_u32(tag, "af_ipv4", enabled ? 1u : 0u);
    }
    if (afi == ISIS_AFI_IPV6)
    {
        return db_update_instance_field_u32(tag, "af_ipv6", enabled ? 1u : 0u);
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

    db_condition_t cond = {
        .field_name = "tag",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)tag),
    };
    db_filter_t filter = {
        .conditions = &cond,
        .num_conditions = 1,
    };

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, &filter, &result);
    db_value_free(&cond.value);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    db_row_t *row = result->rows[0];
    int enabled = 0;
    if (afi == ISIS_AFI_IPV4)
    {
        enabled = db_row_get_int(row, "af_ipv4", 1) ? 1 : 0;
    }
    else if (afi == ISIS_AFI_IPV6)
    {
        enabled = db_row_get_int(row, "af_ipv6", 1) ? 1 : 0;
    }
    else
    {
        db_result_free(result);
        return ERRCODE_FAIL;
    }

    *enabled_out = enabled;
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int isis_db_set_interface_af_cfg(uint32_t tag, const char *ifname, uint16_t afi, const isis_if_af_cfg_t *cfg)
{
    if (tag == 0u || !ifname || ifname[0] == '\0' || !cfg || (afi != ISIS_AFI_IPV4 && afi != ISIS_AFI_IPV6))
    {
        return ERRCODE_FAIL;
    }

    isis_if_af_cfg_t safe = *cfg;
    isis_if_af_cfg_sanitize(&safe);

    if (!safe.enabled)
    {
        return isis_db_del_interface_af(tag, ifname, afi);
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "tag", .op = DB_CMP_EQ, .value = db_value_int((int64_t)tag)},
        {.field_name = "ifname", .op = DB_CMP_EQ, .value = db_value_text(ifname)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)afi)},
    };
    db_filter_t filter = {
        .conditions = conds,
        .num_conditions = G_N_ELEMENTS(conds),
    };

    db_record_t *rec = db_record_new();
    if (!rec)
    {
        db_value_free(&conds[0].value);
        db_value_free(&conds[1].value);
        db_value_free(&conds[2].value);
        return ERRCODE_FAIL;
    }

    db_record_set_int(rec, "tag", (int64_t)tag);
    db_record_set_text(rec, "ifname", ifname);
    db_record_set_int(rec, "afi", (int64_t)afi);
    db_record_set_int(rec, "enabled", safe.enabled ? 1 : 0);
    db_record_set_int(rec, "metric", (int64_t)safe.metric);
    db_record_set_int(rec, "hello_interval", (int64_t)safe.hello_interval);
    db_record_set_int(rec, "hold_multiplier", (int64_t)safe.hold_multiplier);
    db_record_set_int(rec, "passive", (int64_t)(safe.passive ? 1 : 0));

    int rc = db_rpc_upsert(ctx, ISIS_TABLE_INTERFACE, rec, &filter);

    db_record_free(rec);
    db_value_free(&conds[0].value);
    db_value_free(&conds[1].value);
    db_value_free(&conds[2].value);
    return rc;
}

int isis_db_get_interface_af_cfg(uint32_t tag, const char *ifname, uint16_t afi, isis_if_af_cfg_t *cfg_out)
{
    if (tag == 0u || !ifname || ifname[0] == '\0' || (afi != ISIS_AFI_IPV4 && afi != ISIS_AFI_IPV6))
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "tag", .op = DB_CMP_EQ, .value = db_value_int((int64_t)tag)},
        {.field_name = "ifname", .op = DB_CMP_EQ, .value = db_value_text(ifname)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)afi)},
    };
    db_filter_t filter = {
        .conditions = conds,
        .num_conditions = G_N_ELEMENTS(conds),
    };

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, ISIS_TABLE_INTERFACE, NULL, 0, &filter, &result);
    db_value_free(&conds[0].value);
    db_value_free(&conds[1].value);
    db_value_free(&conds[2].value);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    if (cfg_out)
    {
        isis_if_af_cfg_defaults(cfg_out);
        isis_if_apply_row_to_af(result->rows[0], cfg_out);
    }

    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int isis_db_del_interface_af(uint32_t tag, const char *ifname, uint16_t afi)
{
    if (tag == 0u || !ifname || ifname[0] == '\0' || (afi != ISIS_AFI_IPV4 && afi != ISIS_AFI_IPV6))
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "tag", .op = DB_CMP_EQ, .value = db_value_int((int64_t)tag)},
        {.field_name = "ifname", .op = DB_CMP_EQ, .value = db_value_text(ifname)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)afi)},
    };
    db_filter_t filter = {
        .conditions = conds,
        .num_conditions = G_N_ELEMENTS(conds),
    };

    (void)db_rpc_delete(ctx, ISIS_TABLE_INTERFACE, &filter);

    db_value_free(&conds[0].value);
    db_value_free(&conds[1].value);
    db_value_free(&conds[2].value);
    return ERRCODE_SUCCESS;
}

int isis_db_set_interface_cfg(uint32_t tag, const char *ifname, const isis_if_cfg_t *cfg)
{
    if (tag == 0u || !ifname || ifname[0] == '\0' || !cfg)
    {
        return ERRCODE_FAIL;
    }

    isis_if_cfg_t safe = *cfg;
    if (safe.ifname[0] == '\0')
    {
        g_strlcpy(safe.ifname, ifname, sizeof(safe.ifname));
    }
    isis_if_af_cfg_sanitize(&safe.v4);
    isis_if_af_cfg_sanitize(&safe.v6);

    int rc4 = isis_db_set_interface_af_cfg(tag, ifname, ISIS_AFI_IPV4, &safe.v4);
    int rc6 = isis_db_set_interface_af_cfg(tag, ifname, ISIS_AFI_IPV6, &safe.v6);
    return (rc4 == ERRCODE_SUCCESS && rc6 == ERRCODE_SUCCESS) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int isis_db_get_interface_cfg(uint32_t tag, const char *ifname, isis_if_cfg_t *cfg_out)
{
    if (tag == 0u || !ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "tag", .op = DB_CMP_EQ, .value = db_value_int((int64_t)tag)},
        {.field_name = "ifname", .op = DB_CMP_EQ, .value = db_value_text(ifname)},
    };
    db_filter_t filter = {
        .conditions = conds,
        .num_conditions = G_N_ELEMENTS(conds),
    };

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, ISIS_TABLE_INTERFACE, NULL, 0, &filter, &result);
    db_value_free(&conds[0].value);
    db_value_free(&conds[1].value);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    if (cfg_out)
    {
        isis_if_cfg_defaults(cfg_out, ifname);
        for (uint32_t i = 0u; i < result->num_rows; ++i)
        {
            db_row_t *row = result->rows[i];
            uint16_t afi = (uint16_t)db_row_get_int(row, "afi", 0);
            if (afi == ISIS_AFI_IPV4)
            {
                isis_if_apply_row_to_af(row, &cfg_out->v4);
            }
            else if (afi == ISIS_AFI_IPV6)
            {
                isis_if_apply_row_to_af(row, &cfg_out->v6);
            }
            else
            {
                /* legacy rows without AFI: mirror to both families */
                isis_if_apply_row_to_af(row, &cfg_out->v4);
                isis_if_apply_row_to_af(row, &cfg_out->v6);
            }
        }
    }

    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int isis_db_set_interface(uint32_t tag, const char *ifname, uint32_t metric)
{
    isis_if_cfg_t cfg;
    isis_if_cfg_defaults(&cfg, ifname);
    cfg.v4.enabled = 1u;
    cfg.v4.metric = metric;
    isis_if_af_cfg_sanitize(&cfg.v4);
    return isis_db_set_interface_cfg(tag, ifname, &cfg);
}

int isis_db_del_interface(uint32_t tag, const char *ifname)
{
    if (tag == 0u || !ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "tag", .op = DB_CMP_EQ, .value = db_value_int((int64_t)tag)},
        {.field_name = "ifname", .op = DB_CMP_EQ, .value = db_value_text(ifname)},
    };
    db_filter_t filter = {
        .conditions = conds,
        .num_conditions = G_N_ELEMENTS(conds),
    };

    (void)db_rpc_delete(ctx, ISIS_TABLE_INTERFACE, &filter);

    db_value_free(&conds[0].value);
    db_value_free(&conds[1].value);
    return ERRCODE_SUCCESS;
}

int isis_db_get_interface(uint32_t tag, const char *ifname, uint32_t *metric_out)
{
    isis_if_cfg_t cfg;
    if (isis_db_get_interface_cfg(tag, ifname, &cfg) != ERRCODE_SUCCESS || !cfg.v4.enabled)
    {
        return ERRCODE_FAIL;
    }
    if (metric_out)
    {
        *metric_out = cfg.v4.metric;
    }
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
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result ||
        result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    uint32_t best = 0u;
    for (uint32_t i = 0; i < result->num_rows; i++)
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

int isis_db_restore(void)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_result_t *inst_result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, NULL, &inst_result) == ERRCODE_SUCCESS && inst_result)
    {
        for (uint32_t i = 0; i < inst_result->num_rows; i++)
        {
            db_row_t *row = inst_result->rows[i];
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
        db_result_free(inst_result);
    }

    db_result_t *if_result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INTERFACE, NULL, 0, NULL, &if_result) == ERRCODE_SUCCESS && if_result)
    {
        GHashTable *agg = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_restore_if_item_free);
        if (!agg)
        {
            db_result_free(if_result);
            return ERRCODE_FAIL;
        }

        for (uint32_t i = 0; i < if_result->num_rows; i++)
        {
            db_row_t *row = if_result->rows[i];
            uint32_t tag = (uint32_t)db_row_get_int(row, "tag", 0);
            const char *ifname = db_row_get_text(row, "ifname", NULL);
            if (tag == 0u || !ifname || ifname[0] == '\0')
            {
                continue;
            }

            char key[IF_LOGICAL_NAME_MAX + 32];
            g_snprintf(key, sizeof(key), "%u|%s", tag, ifname);

            isis_restore_if_item_t *item = (isis_restore_if_item_t *)g_hash_table_lookup(agg, key);
            if (!item)
            {
                item = g_malloc0(sizeof(*item));
                if (!item)
                {
                    continue;
                }
                item->tag = tag;
                isis_if_cfg_defaults(&item->cfg, ifname);
                g_hash_table_replace(agg, g_strdup(key), item);
            }

            uint16_t afi = (uint16_t)db_row_get_int(row, "afi", 0);
            if (afi == ISIS_AFI_IPV4)
            {
                isis_if_apply_row_to_af(row, &item->cfg.v4);
            }
            else if (afi == ISIS_AFI_IPV6)
            {
                isis_if_apply_row_to_af(row, &item->cfg.v6);
            }
            else
            {
                /* legacy rows without AFI: mirror to both families */
                isis_if_apply_row_to_af(row, &item->cfg.v4);
                isis_if_apply_row_to_af(row, &item->cfg.v6);
            }
        }

        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, agg);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            (void)key;
            isis_restore_if_item_t *item = (isis_restore_if_item_t *)value;
            if (!item || !isis_if_cfg_any_enabled(&item->cfg))
            {
                continue;
            }

            isis_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.op = ISIS_APPLY_OP_IF_SET;
            apply.u.if_set.tag = item->tag;
            apply.u.if_set.cfg = item->cfg;
            (void)isis_worker_dispatch_apply(&apply);
        }

        g_hash_table_destroy(agg);
        db_result_free(if_result);
    }

    return ERRCODE_SUCCESS;
}
