/**
 * @file   lldp_db_interface.c
 * @brief  LLDP interface 表
 * @author jhb
 * @date   2026/06/07
 */
#include <string.h>

#include "errcode.h"
#include "lldp_db_internal.h"
#include "work/lldp_worker.h"

static const db_column_def_t LLDP_IF_COLS[] = {
    {"ifname", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"enabled", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"admin_status", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "3"},
    {"tx_interval_sec", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"hold_multiplier", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"port_desc", DB_TYPE_TEXT, 0, "''"},
};

const db_table_def_t LLDP_IF_TABLE = {
    .table_name = LLDP_TABLE_INTERFACE,
    .cols = LLDP_IF_COLS,
    .num_cols = G_N_ELEMENTS(LLDP_IF_COLS),
};

gboolean lldp_db_interface_is_implicit_default(const lldp_if_cfg_t *cfg)
{
    return cfg && cfg->enabled != 0u && cfg->admin_status == LLDP_IF_ADMIN_TX_RX && cfg->tx_interval_sec == 0u &&
           cfg->hold_multiplier == 0u && cfg->port_desc[0] == '\0';
}

int lldp_db_prune_implicit_default_interfaces(void)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, LLDP_TABLE_INTERFACE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }
    if (!result)
    {
        return ERRCODE_SUCCESS;
    }

    GPtrArray *stale_ifnames = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *hidden_ifnames = g_ptr_array_new_with_free_func(g_free);
    for (uint32_t i = 0; i < result->num_rows; ++i)
    {
        db_row_t *row = result->rows[i];
        lldp_if_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.enabled = (uint8_t)(db_row_get_int(row, "enabled", 1) ? 1 : 0);
        cfg.admin_status = (uint8_t)db_row_get_int(row, "admin_status", LLDP_IF_ADMIN_TX_RX);
        cfg.tx_interval_sec = (uint32_t)db_row_get_int(row, "tx_interval_sec", 0);
        cfg.hold_multiplier = (uint32_t)db_row_get_int(row, "hold_multiplier", 0);
        g_strlcpy(cfg.port_desc, db_row_get_text(row, "port_desc", ""), sizeof(cfg.port_desc));
        const gboolean has_hidden_values = cfg.tx_interval_sec != 0u || cfg.hold_multiplier != 0u;
        cfg.tx_interval_sec = 0u;
        cfg.hold_multiplier = 0u;
        const char *ifname = db_row_get_text(row, "ifname", "");
        if (lldp_db_interface_is_implicit_default(&cfg))
        {
            if (ifname[0] != '\0')
            {
                g_ptr_array_add(stale_ifnames, g_strdup(ifname));
            }
        }
        else if (has_hidden_values && ifname[0] != '\0')
        {
            g_ptr_array_add(hidden_ifnames, g_strdup(ifname));
        }
    }
    db_result_free(result);

    int rc = ERRCODE_SUCCESS;
    for (guint i = 0; i < hidden_ifnames->len; ++i)
    {
        db_filter_builder_t pk;
        lldp_db_if_pk(&pk, g_ptr_array_index(hidden_ifnames, i));
        db_col_t defaults[] = {
            DB_COL_INT("tx_interval_sec", 0),
            DB_COL_INT("hold_multiplier", 0),
        };
        int rows = db_rpc_update_cols(ctx, LLDP_TABLE_INTERFACE, &pk.filter, defaults, G_N_ELEMENTS(defaults));
        db_filter_clear(&pk);
        if (rows < 0)
        {
            rc = ERRCODE_FAIL;
            break;
        }
    }
    for (guint i = 0; i < stale_ifnames->len; ++i)
    {
        db_filter_builder_t pk;
        lldp_db_if_pk(&pk, g_ptr_array_index(stale_ifnames, i));
        int rows = db_rpc_delete(ctx, LLDP_TABLE_INTERFACE, &pk.filter);
        db_filter_clear(&pk);
        if (rows < 0)
        {
            rc = ERRCODE_FAIL;
            break;
        }
    }
    g_ptr_array_free(hidden_ifnames, TRUE);
    g_ptr_array_free(stale_ifnames, TRUE);
    return rc;
}

int lldp_db_set_interface(const char *ifname, const lldp_if_cfg_t *cfg)
{
    if (!ifname || ifname[0] == '\0' || !cfg)
    {
        return ERRCODE_FAIL;
    }
    /*
     * Worker 会为合格接口隐式创建 enabled/txrx 默认状态；相同的 DB 行不含
     * 任何可回放差异。恢复到该状态时删除 override，避免 undo 反而造 marker。
     * enabled=0 的 negative override 仍然是合法配置，必须保留。
     */
    if (lldp_db_interface_is_implicit_default(cfg))
    {
        return lldp_db_del_interface(ifname);
    }

    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    /*
     * 先发布 revive marker，再写接口配置。这样并发配置快照最多把 LLDP
     * 保守地视为 required，不会在接口行已落库而 marker 尚未建立的窗口漏采。
     */
    if (lldp_db_ensure_proto_marker() != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    lldp_db_if_pk(&pk, ifname);

    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, LLDP_TABLE_INTERFACE, &pk.filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        (void)lldp_db_sync_proto_marker();
        return ERRCODE_FAIL;
    }

    if (exists)
    {
        db_col_t cols[] = {
            DB_COL_INT("enabled", cfg->enabled ? 1 : 0),
            DB_COL_INT("admin_status", (int64_t)cfg->admin_status),
            DB_COL_INT("tx_interval_sec", (int64_t)cfg->tx_interval_sec),
            DB_COL_INT("hold_multiplier", (int64_t)cfg->hold_multiplier),
            DB_COL_TEXT("port_desc", cfg->port_desc),
        };
        int rows = db_rpc_update_cols(ctx, LLDP_TABLE_INTERFACE, &pk.filter, cols, G_N_ELEMENTS(cols));
        db_filter_clear(&pk);
        if (rows < 0)
        {
            (void)lldp_db_sync_proto_marker();
            return ERRCODE_FAIL;
        }
        return ERRCODE_SUCCESS;
    }

    db_filter_clear(&pk);

    db_col_t cols[] = {
        DB_COL_TEXT("ifname", ifname),
        DB_COL_INT("enabled", cfg->enabled ? 1 : 0),
        DB_COL_INT("admin_status", (int64_t)cfg->admin_status),
        DB_COL_INT("tx_interval_sec", (int64_t)cfg->tx_interval_sec),
        DB_COL_INT("hold_multiplier", (int64_t)cfg->hold_multiplier),
        DB_COL_TEXT("port_desc", cfg->port_desc),
    };
    if (db_rpc_insert_cols(ctx, LLDP_TABLE_INTERFACE, cols, G_N_ELEMENTS(cols)) != ERRCODE_SUCCESS)
    {
        (void)lldp_db_sync_proto_marker();
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int lldp_db_del_interface(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    lldp_db_if_pk(&pk, ifname);
    int rc = db_rpc_delete(ctx, LLDP_TABLE_INTERFACE, &pk.filter);
    db_filter_clear(&pk);
    if (rc < 0)
    {
        return ERRCODE_FAIL;
    }
    return lldp_db_sync_proto_marker();
}

int lldp_db_get_interface(const char *ifname, lldp_if_cfg_t *cfg_out)
{
    if (!ifname || !cfg_out)
    {
        return ERRCODE_FAIL;
    }
    memset(cfg_out, 0, sizeof(*cfg_out));

    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    lldp_db_if_pk(&pk, ifname);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, LLDP_TABLE_INTERFACE, NULL, 0, &pk.filter, &result);
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
    g_strlcpy(cfg_out->ifname, db_row_get_text(row, "ifname", ""), sizeof(cfg_out->ifname));
    cfg_out->enabled = (uint8_t)(db_row_get_int(row, "enabled", 0) ? 1 : 0);
    cfg_out->admin_status = (uint8_t)db_row_get_int(row, "admin_status", LLDP_IF_ADMIN_TX_RX);
    cfg_out->tx_interval_sec = (uint32_t)db_row_get_int(row, "tx_interval_sec", 0);
    cfg_out->hold_multiplier = (uint32_t)db_row_get_int(row, "hold_multiplier", 0);
    g_strlcpy(cfg_out->port_desc, db_row_get_text(row, "port_desc", ""), sizeof(cfg_out->port_desc));
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

void lldp_db_restore_interfaces(void)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, LLDP_TABLE_INTERFACE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *ifname = db_row_get_text(row, "ifname", NULL);
        if (!ifname || ifname[0] == '\0')
        {
            continue;
        }
        lldp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = LLDP_APPLY_OP_IF_SET;
        g_strlcpy(apply.u.if_set.ifname, ifname, sizeof(apply.u.if_set.ifname));
        apply.u.if_set.enabled = db_row_get_int(row, "enabled", 0) ? 1u : 0u;
        apply.u.if_set.admin_status = (uint8_t)db_row_get_int(row, "admin_status", LLDP_IF_ADMIN_TX_RX);
        apply.u.if_set.tx_interval_sec = (uint32_t)db_row_get_int(row, "tx_interval_sec", 0);
        apply.u.if_set.hold_multiplier = (uint32_t)db_row_get_int(row, "hold_multiplier", 0);
        g_strlcpy(apply.u.if_set.port_desc, db_row_get_text(row, "port_desc", ""), sizeof(apply.u.if_set.port_desc));
        (void)lldp_worker_dispatch_apply(&apply);
    }

    db_result_free(result);
}
