/**
 * @file   isis_db_interface.c
 * @brief  ISIS interface 表：schema、CRUD、启动恢复
 * @author jhb
 * @date   2026/04/26
 */

#include <string.h>

#include "errcode.h"
#include "isis_db_internal.h"

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

const db_table_def_t ISIS_IF_TABLE = {
    .table_name = ISIS_TABLE_INTERFACE,
    .cols = ISIS_IF_COLS,
    .num_cols = G_N_ELEMENTS(ISIS_IF_COLS),
};

typedef struct isis_restore_if_item
{
    uint32_t tag;
    isis_if_cfg_t cfg;
} isis_restore_if_item_t;

static void isis_restore_if_item_free(gpointer data)
{
    g_free(data);
}

int isis_db_set_interface_af_cfg(uint32_t tag, const char *ifname, uint16_t afi, const isis_if_af_cfg_t *cfg)
{
    if (tag == 0u || !ifname || ifname[0] == '\0' || !cfg || (afi != ISIS_AFI_IPV4 && afi != ISIS_AFI_IPV6))
    {
        return ERRCODE_FAIL;
    }

    isis_if_af_cfg_t safe = *cfg;
    isis_db_if_af_cfg_sanitize(&safe);
    if (!safe.enabled)
    {
        return isis_db_del_interface_af(tag, ifname, afi);
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    isis_db_interface_af_pk(&pk, tag, ifname, afi);

    /* 行可能不存在（首次配置）：先查存在性，分别走 UPDATE / INSERT，避免依赖 upsert */
    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, ISIS_TABLE_INTERFACE, &pk.filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        return ERRCODE_FAIL;
    }

    if (exists)
    {
        db_col_t cols[] = {
            DB_COL_INT("enabled", safe.enabled ? 1 : 0),       DB_COL_INT("metric", safe.metric),
            DB_COL_INT("hello_interval", safe.hello_interval), DB_COL_INT("hold_multiplier", safe.hold_multiplier),
            DB_COL_INT("passive", safe.passive ? 1 : 0),
        };
        int rows = db_rpc_update_cols(ctx, ISIS_TABLE_INTERFACE, &pk.filter, cols, G_N_ELEMENTS(cols));
        db_filter_clear(&pk);
        return (rows > 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    db_filter_clear(&pk);

    db_col_t cols[] = {
        DB_COL_INT("tag", tag),
        DB_COL_TEXT("ifname", ifname),
        DB_COL_INT("afi", afi),
        DB_COL_INT("enabled", safe.enabled ? 1 : 0),
        DB_COL_INT("metric", safe.metric),
        DB_COL_INT("hello_interval", safe.hello_interval),
        DB_COL_INT("hold_multiplier", safe.hold_multiplier),
        DB_COL_INT("passive", safe.passive ? 1 : 0),
    };
    return db_rpc_insert_cols(ctx, ISIS_TABLE_INTERFACE, cols, G_N_ELEMENTS(cols));
}

int isis_db_get_interface_af_cfg(uint32_t tag, const char *ifname, uint16_t afi, isis_if_af_cfg_t *cfg_out)
{
    if (tag == 0u || !ifname || ifname[0] == '\0' || (afi != ISIS_AFI_IPV4 && afi != ISIS_AFI_IPV6))
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    isis_db_interface_af_pk(&pk, tag, ifname, afi);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, ISIS_TABLE_INTERFACE, NULL, 0, &pk.filter, &result);
    db_filter_clear(&pk);
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
        isis_db_if_af_cfg_defaults(cfg_out);
        isis_db_if_apply_row_to_af(result->rows[0], cfg_out);
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
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    isis_db_interface_af_pk(&pk, tag, ifname, afi);
    (void)db_rpc_delete(ctx, ISIS_TABLE_INTERFACE, &pk.filter);
    db_filter_clear(&pk);
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
    isis_db_if_af_cfg_sanitize(&safe.v4);
    isis_db_if_af_cfg_sanitize(&safe.v6);

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
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    isis_db_interface_key_pk(&pk, tag, ifname);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, ISIS_TABLE_INTERFACE, NULL, 0, &pk.filter, &result);
    db_filter_clear(&pk);
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
        isis_db_if_cfg_defaults(cfg_out, ifname);
        for (uint32_t i = 0u; i < result->num_rows; ++i)
        {
            db_row_t *row = result->rows[i];
            uint16_t afi = (uint16_t)db_row_get_int(row, "afi", 0);
            if (afi == ISIS_AFI_IPV4)
            {
                isis_db_if_apply_row_to_af(row, &cfg_out->v4);
            }
            else if (afi == ISIS_AFI_IPV6)
            {
                isis_db_if_apply_row_to_af(row, &cfg_out->v6);
            }
            else
            {
                isis_db_if_apply_row_to_af(row, &cfg_out->v4);
                isis_db_if_apply_row_to_af(row, &cfg_out->v6);
            }
        }
    }

    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int isis_db_set_interface(uint32_t tag, const char *ifname, uint32_t metric)
{
    isis_if_cfg_t cfg;
    isis_db_if_cfg_defaults(&cfg, ifname);
    cfg.v4.enabled = 1u;
    cfg.v4.metric = metric;
    isis_db_if_af_cfg_sanitize(&cfg.v4);
    return isis_db_set_interface_cfg(tag, ifname, &cfg);
}

int isis_db_del_interface(uint32_t tag, const char *ifname)
{
    if (tag == 0u || !ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    isis_db_interface_key_pk(&pk, tag, ifname);
    (void)db_rpc_delete(ctx, ISIS_TABLE_INTERFACE, &pk.filter);
    db_filter_clear(&pk);
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

void isis_db_restore_interfaces(void)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INTERFACE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    GHashTable *agg = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_restore_if_item_free);
    if (!agg)
    {
        db_result_free(result);
        return;
    }

    for (uint32_t i = 0u; i < result->num_rows; ++i)
    {
        db_row_t *row = result->rows[i];
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
            isis_db_if_cfg_defaults(&item->cfg, ifname);
            g_hash_table_replace(agg, g_strdup(key), item);
        }

        uint16_t afi = (uint16_t)db_row_get_int(row, "afi", 0);
        if (afi == ISIS_AFI_IPV4)
        {
            isis_db_if_apply_row_to_af(row, &item->cfg.v4);
        }
        else if (afi == ISIS_AFI_IPV6)
        {
            isis_db_if_apply_row_to_af(row, &item->cfg.v6);
        }
        else
        {
            isis_db_if_apply_row_to_af(row, &item->cfg.v4);
            isis_db_if_apply_row_to_af(row, &item->cfg.v6);
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
    db_result_free(result);
}
