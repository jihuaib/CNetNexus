#include "srv6_db.h"

#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "db.h"
#include "errcode.h"
#include "log.h"
#include "srv6_main.h"
#include "work/srv6_worker.h"

static const db_column_def_t SRV6_LOCATOR_COLS[] = {
    {"name", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"prefix", DB_TYPE_TEXT, DB_COL_NOT_NULL | DB_COL_UNIQUE, NULL},
    {"prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"function_bits", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "16"},
};

static const db_table_def_t SRV6_LOCATOR_TABLE_DEF = {
    .table_name = SRV6_TABLE_LOCATOR,
    .cols = SRV6_LOCATOR_COLS,
    .num_cols = G_N_ELEMENTS(SRV6_LOCATOR_COLS),
};

static const db_column_def_t SRV6_SID_COLS[] = {
    {"binding_key", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"locator_name", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"behavior", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"owner_module_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"owner_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"function_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"sid", DB_TYPE_TEXT, DB_COL_NOT_NULL | DB_COL_UNIQUE, NULL},
};

static const db_table_def_t SRV6_SID_TABLE_DEF = {
    .table_name = SRV6_TABLE_SID,
    .cols = SRV6_SID_COLS,
    .num_cols = G_N_ELEMENTS(SRV6_SID_COLS),
};

static void srv6_db_locator_filter(db_filter_builder_t *filter, const char *name)
{
    db_filter_init(filter);
    db_filter_add_text(filter, "name", name ? name : "");
}

void srv6_db_sid_key_text(const srv6_sid_key_t *key, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0u)
    {
        return;
    }
    if (!key)
    {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, buf_len, "%s|%u|%u|%u|%u", key->locator, key->vrf_id, (unsigned)key->behavior, key->owner_module_id,
             key->owner_id);
}

static void srv6_db_sid_filter(db_filter_builder_t *filter, const srv6_sid_key_t *key)
{
    char binding_key[SRV6_LOCATOR_NAME_MAX + 64u];
    srv6_db_sid_key_text(key, binding_key, sizeof(binding_key));
    db_filter_init(filter);
    db_filter_add_text(filter, "binding_key", binding_key);
}

int srv6_db_init(void)
{
    dev_ipc_context_t *ctx = srv6_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    if (db_rpc_create_table_from_def(ctx, &SRV6_LOCATOR_TABLE_DEF) != ERRCODE_SUCCESS ||
        db_rpc_create_table_from_def(ctx, &SRV6_SID_TABLE_DEF) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: failed to initialize database tables");
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int srv6_db_locator_upsert(const srv6_locator_t *locator)
{
    dev_ipc_context_t *ctx = srv6_local_ipc_ctx();
    if (!ctx || !locator || locator->name[0] == '\0' || locator->prefix.family != AF_INET6)
    {
        return ERRCODE_FAIL;
    }

    char prefix[INET6_ADDRSTRLEN];
    net_addr_to_str(&locator->prefix, prefix, sizeof(prefix));
    db_filter_builder_t filter;
    srv6_db_locator_filter(&filter, locator->name);
    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, SRV6_TABLE_LOCATOR, &filter.filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        db_filter_clear(&filter);
        return ERRCODE_FAIL;
    }

    db_col_t cols[] = {
        DB_COL_TEXT("prefix", prefix),
        DB_COL_INT("prefix_len", locator->prefix_len),
        DB_COL_INT("function_bits", locator->function_bits),
    };
    if (exists)
    {
        rc = db_rpc_update_cols(ctx, SRV6_TABLE_LOCATOR, &filter.filter, cols, G_N_ELEMENTS(cols));
        db_filter_clear(&filter);
        return rc < 0 ? ERRCODE_FAIL : ERRCODE_SUCCESS;
    }
    db_filter_clear(&filter);

    db_col_t insert_cols[] = {
        DB_COL_TEXT("name", locator->name),
        DB_COL_TEXT("prefix", prefix),
        DB_COL_INT("prefix_len", locator->prefix_len),
        DB_COL_INT("function_bits", locator->function_bits),
    };
    return db_rpc_insert_cols(ctx, SRV6_TABLE_LOCATOR, insert_cols, G_N_ELEMENTS(insert_cols));
}

int srv6_db_locator_delete(const char *name)
{
    dev_ipc_context_t *ctx = srv6_local_ipc_ctx();
    if (!ctx || !name)
    {
        return ERRCODE_FAIL;
    }
    db_filter_builder_t filter;
    srv6_db_locator_filter(&filter, name);
    int rows = db_rpc_delete(ctx, SRV6_TABLE_LOCATOR, &filter.filter);
    db_filter_clear(&filter);
    return rows < 0 ? ERRCODE_FAIL : ERRCODE_SUCCESS;
}

int srv6_db_locator_list(GPtrArray **out)
{
    if (!out)
    {
        return ERRCODE_FAIL;
    }
    *out = g_ptr_array_new_with_free_func(g_free);
    db_result_t *result = NULL;
    if (db_rpc_query(srv6_local_ipc_ctx(), SRV6_TABLE_LOCATOR, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        if (result)
        {
            db_result_free(result);
        }
        g_ptr_array_free(*out, TRUE);
        *out = NULL;
        return ERRCODE_FAIL;
    }

    if (result)
    {
        for (uint32_t i = 0; i < result->num_rows; ++i)
        {
            const db_row_t *row = result->rows[i];
            srv6_locator_t *locator = g_new0(srv6_locator_t, 1);
            g_strlcpy(locator->name, db_row_get_text(row, "name", ""), sizeof(locator->name));
            locator->prefix_len = (uint8_t)db_row_get_int(row, "prefix_len", 0);
            locator->function_bits = (uint8_t)db_row_get_int(row, "function_bits", SRV6_DEFAULT_FUNCTION_BITS);
            const char *prefix = db_row_get_text(row, "prefix", "");
            if (locator->name[0] == '\0' || net_addr_from_str(prefix, &locator->prefix) != 0 ||
                locator->prefix.family != AF_INET6 ||
                net_addr_prefix_normalize(&locator->prefix, locator->prefix_len) != 0)
            {
                LOG_WARN("SRV6: ignoring invalid persisted locator row name=%s prefix=%s", locator->name, prefix);
                g_free(locator);
                continue;
            }
            g_ptr_array_add(*out, locator);
        }
        db_result_free(result);
    }
    return ERRCODE_SUCCESS;
}

int srv6_db_sid_insert(const srv6_sid_entry_t *entry)
{
    dev_ipc_context_t *ctx = srv6_local_ipc_ctx();
    if (!ctx || !entry || entry->sid.family != AF_INET6)
    {
        return ERRCODE_FAIL;
    }
    char binding_key[SRV6_LOCATOR_NAME_MAX + 64u];
    char sid[INET6_ADDRSTRLEN];
    srv6_db_sid_key_text(&entry->key, binding_key, sizeof(binding_key));
    net_addr_to_str(&entry->sid, sid, sizeof(sid));
    db_col_t cols[] = {
        DB_COL_TEXT("binding_key", binding_key),
        DB_COL_TEXT("locator_name", entry->key.locator),
        DB_COL_INT("vrf_id", entry->key.vrf_id),
        DB_COL_INT("behavior", entry->key.behavior),
        DB_COL_INT("owner_module_id", entry->key.owner_module_id),
        DB_COL_INT("owner_id", entry->key.owner_id),
        DB_COL_INT("function_id", entry->function_id),
        DB_COL_TEXT("sid", sid),
    };
    return db_rpc_insert_cols(ctx, SRV6_TABLE_SID, cols, G_N_ELEMENTS(cols));
}

int srv6_db_sid_delete(const srv6_sid_key_t *key)
{
    dev_ipc_context_t *ctx = srv6_local_ipc_ctx();
    if (!ctx || !key)
    {
        return ERRCODE_FAIL;
    }
    db_filter_builder_t filter;
    srv6_db_sid_filter(&filter, key);
    int rows = db_rpc_delete(ctx, SRV6_TABLE_SID, &filter.filter);
    db_filter_clear(&filter);
    return rows < 0 ? ERRCODE_FAIL : ERRCODE_SUCCESS;
}

int srv6_db_sid_list(GPtrArray **out)
{
    if (!out)
    {
        return ERRCODE_FAIL;
    }
    *out = g_ptr_array_new_with_free_func(g_free);
    db_result_t *result = NULL;
    if (db_rpc_query(srv6_local_ipc_ctx(), SRV6_TABLE_SID, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        if (result)
        {
            db_result_free(result);
        }
        g_ptr_array_free(*out, TRUE);
        *out = NULL;
        return ERRCODE_FAIL;
    }

    if (result)
    {
        for (uint32_t i = 0; i < result->num_rows; ++i)
        {
            const db_row_t *row = result->rows[i];
            srv6_sid_entry_t *entry = g_new0(srv6_sid_entry_t, 1);
            g_strlcpy(entry->key.locator, db_row_get_text(row, "locator_name", ""), sizeof(entry->key.locator));
            entry->key.vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", 0);
            entry->key.behavior = (uint16_t)db_row_get_int(row, "behavior", 0);
            entry->key.owner_module_id = (uint32_t)db_row_get_int(row, "owner_module_id", 0);
            entry->key.owner_id = (uint32_t)db_row_get_int(row, "owner_id", 0);
            entry->function_id = (uint32_t)db_row_get_int(row, "function_id", 0);
            entry->prefix_len = 128u;
            const char *sid = db_row_get_text(row, "sid", "");
            if (entry->key.locator[0] == '\0' || entry->function_id == 0u || net_addr_from_str(sid, &entry->sid) != 0 ||
                entry->sid.family != AF_INET6)
            {
                LOG_WARN("SRV6: ignoring invalid persisted SID row locator=%s sid=%s", entry->key.locator, sid);
                g_free(entry);
                continue;
            }
            g_ptr_array_add(*out, entry);
        }
        db_result_free(result);
    }
    return ERRCODE_SUCCESS;
}

int srv6_db_restore(void)
{
    GPtrArray *locators = NULL;
    GPtrArray *bindings = NULL;

    /* ROUTE 的 RIB 跨协议进程重启保留。locator upsert 在路由下发与 DB 提交之间
     * 崩溃时，可能留下 DB 中不存在的黑洞 aggregate，更新时还可能同时留下
     * 旧/新前缀。冷恢复必须先通过已认证、严格限定在 public IPv6 + protocol=SRV6
     * 的 ROUTE RPC 清理所有者路径，再以 DB 为唯一真值重放。即使 DB 查询随后失败，
     * 也不能继续保留未持久化的黑洞路由。 */
    if (srv6_worker_prepare_restore() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SRV6: failed to clear owned locator routes before DB restore");
        return ERRCODE_FAIL;
    }

    if (srv6_db_locator_list(&locators) != ERRCODE_SUCCESS || srv6_db_sid_list(&bindings) != ERRCODE_SUCCESS)
    {
        if (locators)
        {
            g_ptr_array_free(locators, TRUE);
        }
        if (bindings)
        {
            g_ptr_array_free(bindings, TRUE);
        }
        return ERRCODE_FAIL;
    }
    int rc = srv6_worker_restore(locators, bindings);
    g_ptr_array_free(locators, TRUE);
    g_ptr_array_free(bindings, TRUE);
    return rc;
}

int srv6_db_delete_config(void)
{
    dev_ipc_context_t *ctx = srv6_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    int sid_rows = db_rpc_delete(ctx, SRV6_TABLE_SID, NULL);
    if (sid_rows < 0)
    {
        return ERRCODE_FAIL;
    }
    int locator_rows = db_rpc_delete(ctx, SRV6_TABLE_LOCATOR, NULL);
    if (locator_rows < 0)
    {
        return ERRCODE_FAIL;
    }
    LOG_INFO("SRV6: cleared persisted configuration locator=%d sid=%d", locator_rows, sid_rows);
    return ERRCODE_SUCCESS;
}
