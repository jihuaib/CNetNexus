/**
 * @file   ospf_db.c
 * @brief  OSPFv2 persistent configuration and startup restore
 */

#include "ospf_db.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "db.h"
#include "errcode.h"
#include "log.h"
#include "ospf_main.h"
#include "vrf.h"

#define OSPF_VRF_TABLE_INSTANCE "vrf_instance"

static const db_column_def_t OSPF_INSTANCE_COLS[] = {
    {"process_id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL}, {"router_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"admin_up", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},       {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"vrf_name", DB_TYPE_TEXT, DB_COL_NOT_NULL, "public"},
};

static const db_table_def_t OSPF_INSTANCE_TABLE = {
    .table_name = OSPF_TABLE_INSTANCE,
    .cols = OSPF_INSTANCE_COLS,
    .num_cols = G_N_ELEMENTS(OSPF_INSTANCE_COLS),
};

static const db_column_def_t OSPF_AREA_COLS[] = {
    {"id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY | DB_COL_AUTOINCREMENT, NULL},
    {"process_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"area_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
};

static const db_table_def_t OSPF_AREA_TABLE = {
    .table_name = OSPF_TABLE_AREA,
    .cols = OSPF_AREA_COLS,
    .num_cols = G_N_ELEMENTS(OSPF_AREA_COLS),
};

static const db_column_def_t OSPF_INTERFACE_COLS[] = {
    {"id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY | DB_COL_AUTOINCREMENT, NULL},
    {"process_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"ifname", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"enabled", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"area_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"network_type", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"cost", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "10"},
    {"hello_interval", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "10"},
    {"dead_interval", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "40"},
    {"priority", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"passive", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
};

static const db_table_def_t OSPF_INTERFACE_TABLE = {
    .table_name = OSPF_TABLE_INTERFACE,
    .cols = OSPF_INTERFACE_COLS,
    .num_cols = G_N_ELEMENTS(OSPF_INTERFACE_COLS),
};

static void ospf_db_instance_pk(db_filter_builder_t *pk, uint32_t process_id)
{
    db_filter_init(pk);
    db_filter_add_int(pk, "process_id", (int64_t)process_id);
}

static void ospf_db_area_pk(db_filter_builder_t *pk, uint32_t process_id, uint32_t area_id)
{
    db_filter_init(pk);
    db_filter_add_int(pk, "process_id", (int64_t)process_id);
    db_filter_add_int(pk, "area_id", (int64_t)area_id);
}

static void ospf_db_interface_pk(db_filter_builder_t *pk, uint32_t process_id, const char *ifname)
{
    db_filter_init(pk);
    db_filter_add_int(pk, "process_id", (int64_t)process_id);
    db_filter_add_text(pk, "ifname", ifname);
}

static gboolean ospf_db_int_is_u32(int64_t value)
{
    return value >= 0 && (uint64_t)value <= UINT32_MAX;
}

static void ospf_db_if_cfg_defaults(ospf_if_cfg_t *cfg, const char *ifname)
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
    cfg->enabled = 1u;
    cfg->network_type = OSPF_NETWORK_BROADCAST;
    cfg->priority = OSPF_DEFAULT_PRIORITY;
    cfg->cost = OSPF_DEFAULT_COST;
    cfg->hello_interval = OSPF_DEFAULT_HELLO_INTERVAL;
    cfg->dead_interval = OSPF_DEFAULT_DEAD_INTERVAL;
}

static void ospf_db_if_cfg_sanitize(ospf_if_cfg_t *cfg)
{
    if (!cfg)
    {
        return;
    }

    cfg->enabled = cfg->enabled ? 1u : 0u;
    cfg->passive = cfg->passive ? 1u : 0u;
    if (cfg->network_type != OSPF_NETWORK_BROADCAST && cfg->network_type != OSPF_NETWORK_POINT_TO_POINT)
    {
        cfg->network_type = OSPF_NETWORK_BROADCAST;
    }
    if (cfg->cost == 0u)
    {
        cfg->cost = OSPF_DEFAULT_COST;
    }
    if (cfg->hello_interval == 0u)
    {
        cfg->hello_interval = OSPF_DEFAULT_HELLO_INTERVAL;
    }
    if (cfg->dead_interval == 0u)
    {
        cfg->dead_interval = OSPF_DEFAULT_DEAD_INTERVAL;
    }
}

static void ospf_db_if_cfg_from_row(const db_row_t *row, ospf_if_cfg_t *cfg)
{
    if (!row || !cfg)
    {
        return;
    }

    int64_t area_id = db_row_get_int(row, "area_id", OSPF_AREA_BACKBONE);
    int64_t network_type = db_row_get_int(row, "network_type", OSPF_NETWORK_BROADCAST);
    int64_t cost = db_row_get_int(row, "cost", OSPF_DEFAULT_COST);
    int64_t hello_interval = db_row_get_int(row, "hello_interval", OSPF_DEFAULT_HELLO_INTERVAL);
    int64_t dead_interval = db_row_get_int(row, "dead_interval", OSPF_DEFAULT_DEAD_INTERVAL);
    int64_t priority = db_row_get_int(row, "priority", OSPF_DEFAULT_PRIORITY);

    cfg->enabled = db_row_get_int(row, "enabled", 1) ? 1u : 0u;
    cfg->passive = db_row_get_int(row, "passive", 0) ? 1u : 0u;
    cfg->area_id = ospf_db_int_is_u32(area_id) ? (uint32_t)area_id : OSPF_AREA_BACKBONE;
    cfg->network_type =
        (network_type == OSPF_NETWORK_POINT_TO_POINT) ? OSPF_NETWORK_POINT_TO_POINT : OSPF_NETWORK_BROADCAST;
    cfg->cost = (cost > 0 && cost <= OSPF_MAX_COST) ? (uint16_t)cost : OSPF_DEFAULT_COST;
    cfg->hello_interval =
        (hello_interval > 0 && hello_interval <= UINT16_MAX) ? (uint16_t)hello_interval : OSPF_DEFAULT_HELLO_INTERVAL;
    cfg->dead_interval = (dead_interval > 0 && (uint64_t)dead_interval <= UINT32_MAX) ? (uint32_t)dead_interval
                                                                                      : OSPF_DEFAULT_DEAD_INTERVAL;
    cfg->priority = (priority >= 0 && priority <= UINT8_MAX) ? (uint8_t)priority : OSPF_DEFAULT_PRIORITY;
}

static int ospf_db_cleanup_orphan_rows(dev_ipc_context_t *ctx, const char *child_table)
{
    const char *fields[] = {"id", "process_id"};
    db_result_t *result = NULL;
    if (!ctx || !child_table ||
        db_rpc_query(ctx, child_table, fields, G_N_ELEMENTS(fields), NULL, &result) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (!result)
    {
        return ERRCODE_SUCCESS;
    }

    uint32_t removed = 0u;
    int status = ERRCODE_SUCCESS;
    for (uint32_t i = 0u; i < result->num_rows; ++i)
    {
        const db_row_t *row = result->rows[i];
        int64_t row_id = db_row_get_int(row, "id", INT64_MIN);
        int64_t process_id = db_row_get_int(row, "process_id", INT64_MIN);

        db_filter_builder_t parent_pk;
        db_filter_init(&parent_pk);
        db_filter_add_int(&parent_pk, "process_id", process_id);
        gboolean parent_exists = FALSE;
        int rc = db_rpc_exists(ctx, OSPF_TABLE_INSTANCE, &parent_pk.filter, &parent_exists);
        db_filter_clear(&parent_pk);
        if (rc != ERRCODE_SUCCESS)
        {
            LOG_ERROR("OSPF: failed to check parent for %s row %" PRId64, child_table, row_id);
            status = ERRCODE_FAIL;
            continue;
        }
        if (parent_exists)
        {
            continue;
        }

        db_filter_builder_t child_pk;
        db_filter_init(&child_pk);
        db_filter_add_int(&child_pk, "id", row_id);
        int rows = db_rpc_delete(ctx, child_table, &child_pk.filter);
        db_filter_clear(&child_pk);
        if (rows < 0)
        {
            LOG_ERROR("OSPF: failed to remove orphan %s row %" PRId64, child_table, row_id);
            status = ERRCODE_FAIL;
            continue;
        }
        removed += (uint32_t)rows;
    }

    if (removed > 0u)
    {
        LOG_WARN("OSPF: removed %u orphan row(s) from %s", removed, child_table);
    }
    db_result_free(result);
    return status;
}

int ospf_db_init(void)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    static const db_table_def_t *const tables[] = {
        &OSPF_INSTANCE_TABLE,
        &OSPF_AREA_TABLE,
        &OSPF_INTERFACE_TABLE,
    };

    for (size_t i = 0u; i < G_N_ELEMENTS(tables); ++i)
    {
        if (db_rpc_create_table_from_def(ctx, tables[i]) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("OSPF: create table %s failed", tables[i]->table_name);
            return ERRCODE_FAIL;
        }
    }

    /* Versioned names allow a later cascade definition to coexist during schema upgrades. */
    if (db_rpc_create_delete_cascade(ctx, "ospf_instance_delete_interfaces_v1", OSPF_TABLE_INSTANCE, "process_id",
                                     OSPF_TABLE_INTERFACE, "process_id") != ERRCODE_SUCCESS ||
        db_rpc_create_delete_cascade(ctx, "ospf_instance_delete_areas_v1", OSPF_TABLE_INSTANCE, "process_id",
                                     OSPF_TABLE_AREA, "process_id") != ERRCODE_SUCCESS)
    {
        LOG_ERROR("OSPF: create instance delete cascade failed");
        return ERRCODE_FAIL;
    }

    int interface_cleanup_rc = ospf_db_cleanup_orphan_rows(ctx, OSPF_TABLE_INTERFACE);
    int area_cleanup_rc = ospf_db_cleanup_orphan_rows(ctx, OSPF_TABLE_AREA);
    if (interface_cleanup_rc != ERRCODE_SUCCESS || area_cleanup_rc != ERRCODE_SUCCESS)
    {
        LOG_ERROR("OSPF: orphan configuration cleanup failed");
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int ospf_db_set_instance(uint32_t process_id, uint32_t vrf_id, const char *vrf_name)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ospf_db_instance_pk(&pk, process_id);

    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, OSPF_TABLE_INSTANCE, &pk.filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        return ERRCODE_FAIL;
    }

    if (exists)
    {
        db_col_t cols[] = {
            DB_COL_INT("admin_up", 1),
            DB_COL_INT("vrf_id", vrf_id),
            DB_COL_TEXT("vrf_name", vrf_name),
        };
        int rows = db_rpc_update_cols(ctx, OSPF_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
        db_filter_clear(&pk);
        return rows >= 0 ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    db_filter_clear(&pk);
    db_col_t cols[] = {
        DB_COL_INT("process_id", process_id), DB_COL_INT("router_id", 0),        DB_COL_INT("admin_up", 1),
        DB_COL_INT("vrf_id", vrf_id),         DB_COL_TEXT("vrf_name", vrf_name),
    };
    return db_rpc_insert_cols(ctx, OSPF_TABLE_INSTANCE, cols, G_N_ELEMENTS(cols));
}

int ospf_db_del_instance(uint32_t process_id)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t inst_pk;
    ospf_db_instance_pk(&inst_pk, process_id);
    int inst_rows = db_rpc_delete(ctx, OSPF_TABLE_INSTANCE, &inst_pk.filter);
    db_filter_clear(&inst_pk);

    return inst_rows >= 0 ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int ospf_db_set_router_id(uint32_t process_id, uint32_t router_id)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ospf_db_instance_pk(&pk, process_id);
    db_col_t cols[] = {
        DB_COL_INT("router_id", router_id),
    };
    int rows = db_rpc_update_cols(ctx, OSPF_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);
    return rows > 0 ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int ospf_db_get_instance(uint32_t process_id, uint32_t *router_id_out, uint32_t *vrf_id_out, char *vrf_name_out,
                         size_t vrf_name_out_size)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u)
    {
        return ERRCODE_FAIL;
    }

    if (router_id_out)
    {
        *router_id_out = 0u;
    }
    if (vrf_id_out)
    {
        *vrf_id_out = VRF_PUBLIC_VRF_ID;
    }
    if (vrf_name_out && vrf_name_out_size > 0u)
    {
        g_strlcpy(vrf_name_out, VRF_PUBLIC_VRF_NAME, vrf_name_out_size);
    }

    db_filter_builder_t pk;
    ospf_db_instance_pk(&pk, process_id);
    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, OSPF_TABLE_INSTANCE, NULL, 0, &pk.filter, &result);
    db_filter_clear(&pk);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    int64_t router_id = db_row_get_int(result->rows[0], "router_id", 0);
    if (router_id_out && ospf_db_int_is_u32(router_id))
    {
        *router_id_out = (uint32_t)router_id;
    }
    int64_t vrf_id = db_row_get_int(result->rows[0], "vrf_id", VRF_PUBLIC_VRF_ID);
    if (vrf_id_out && ospf_db_int_is_u32(vrf_id))
    {
        *vrf_id_out = (uint32_t)vrf_id;
    }
    if (vrf_name_out && vrf_name_out_size > 0u)
    {
        g_strlcpy(vrf_name_out, db_row_get_text(result->rows[0], "vrf_name", VRF_PUBLIC_VRF_NAME), vrf_name_out_size);
    }
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int ospf_db_resolve_vrf(const char *vrf_name, uint32_t *vrf_id_out)
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
    int rc = db_rpc_query(ospf_local_ipc_ctx(), OSPF_VRF_TABLE_INSTANCE, fields, G_N_ELEMENTS(fields), &filter.filter,
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
    if (!ospf_db_int_is_u32(vrf_id) || vrf_id == VRF_PUBLIC_VRF_ID)
    {
        return ERRCODE_FAIL;
    }
    *vrf_id_out = (uint32_t)vrf_id;
    return ERRCODE_SUCCESS;
}

int ospf_db_area_exists(uint32_t process_id, uint32_t area_id, gboolean *exists)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u || !exists)
    {
        return ERRCODE_FAIL;
    }

    *exists = FALSE;
    db_filter_builder_t pk;
    ospf_db_area_pk(&pk, process_id, area_id);
    int rc = db_rpc_exists(ctx, OSPF_TABLE_AREA, &pk.filter, exists);
    db_filter_clear(&pk);
    return rc;
}

int ospf_db_set_area(uint32_t process_id, uint32_t area_id)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u || ospf_db_get_instance(process_id, NULL, NULL, NULL, 0u) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    gboolean exists = FALSE;
    if (ospf_db_area_exists(process_id, area_id, &exists) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (exists)
    {
        return ERRCODE_SUCCESS;
    }

    db_col_t cols[] = {
        DB_COL_INT("process_id", process_id),
        DB_COL_INT("area_id", area_id),
    };
    return db_rpc_insert_cols(ctx, OSPF_TABLE_AREA, cols, G_N_ELEMENTS(cols));
}

int ospf_db_area_in_use(uint32_t process_id, uint32_t area_id, char *ifname_out, size_t ifname_out_size)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (ifname_out && ifname_out_size > 0u)
    {
        ifname_out[0] = '\0';
    }
    if (!ctx || process_id == 0u || (ifname_out && ifname_out_size == 0u))
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t filter;
    ospf_db_area_pk(&filter, process_id, area_id);
    const char *fields[] = {"ifname"};
    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, OSPF_TABLE_INTERFACE, fields, G_N_ELEMENTS(fields), &filter.filter, &result);
    db_filter_clear(&filter);
    if (rc != ERRCODE_SUCCESS)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    int in_use = result && result->num_rows > 0u;
    if (in_use && ifname_out)
    {
        const char *ifname = db_row_get_text(result->rows[0], "ifname", "");
        g_strlcpy(ifname_out, ifname, ifname_out_size);
    }
    if (result)
    {
        db_result_free(result);
    }
    return in_use;
}

int ospf_db_del_area(uint32_t process_id, uint32_t area_id)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u)
    {
        return ERRCODE_FAIL;
    }

    int in_use = ospf_db_area_in_use(process_id, area_id, NULL, 0u);
    if (in_use != 0)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ospf_db_area_pk(&pk, process_id, area_id);
    int rows = db_rpc_delete(ctx, OSPF_TABLE_AREA, &pk.filter);
    db_filter_clear(&pk);
    return rows >= 0 ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int ospf_db_set_interface(uint32_t process_id, const ospf_if_cfg_t *cfg)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u || !cfg || cfg->ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    ospf_if_cfg_t safe = *cfg;
    safe.ifname[sizeof(safe.ifname) - 1u] = '\0';
    ospf_db_if_cfg_sanitize(&safe);

    gboolean area_exists = FALSE;
    if (ospf_db_area_exists(process_id, safe.area_id, &area_exists) != ERRCODE_SUCCESS || !area_exists)
    {
        LOG_WARN("OSPF: process %u area %u is not configured", process_id, safe.area_id);
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ospf_db_interface_pk(&pk, process_id, safe.ifname);

    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, OSPF_TABLE_INTERFACE, &pk.filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        return ERRCODE_FAIL;
    }

    db_col_t cfg_cols[] = {
        DB_COL_INT("enabled", safe.enabled),
        DB_COL_INT("area_id", safe.area_id),
        DB_COL_INT("network_type", safe.network_type),
        DB_COL_INT("cost", safe.cost),
        DB_COL_INT("hello_interval", safe.hello_interval),
        DB_COL_INT("dead_interval", safe.dead_interval),
        DB_COL_INT("priority", safe.priority),
        DB_COL_INT("passive", safe.passive),
    };

    if (exists)
    {
        int rows = db_rpc_update_cols(ctx, OSPF_TABLE_INTERFACE, &pk.filter, cfg_cols, G_N_ELEMENTS(cfg_cols));
        db_filter_clear(&pk);
        return rows >= 0 ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    db_filter_clear(&pk);
    db_col_t insert_cols[] = {
        DB_COL_INT("process_id", process_id),
        DB_COL_TEXT("ifname", safe.ifname),
        DB_COL_INT("enabled", safe.enabled),
        DB_COL_INT("area_id", safe.area_id),
        DB_COL_INT("network_type", safe.network_type),
        DB_COL_INT("cost", safe.cost),
        DB_COL_INT("hello_interval", safe.hello_interval),
        DB_COL_INT("dead_interval", safe.dead_interval),
        DB_COL_INT("priority", safe.priority),
        DB_COL_INT("passive", safe.passive),
    };
    return db_rpc_insert_cols(ctx, OSPF_TABLE_INTERFACE, insert_cols, G_N_ELEMENTS(insert_cols));
}

int ospf_db_get_interface(uint32_t process_id, const char *ifname, ospf_if_cfg_t *cfg_out)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u || !ifname || ifname[0] == '\0' || !cfg_out)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ospf_db_interface_pk(&pk, process_id, ifname);
    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, OSPF_TABLE_INTERFACE, NULL, 0, &pk.filter, &result);
    db_filter_clear(&pk);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    ospf_db_if_cfg_defaults(cfg_out, ifname);
    ospf_db_if_cfg_from_row(result->rows[0], cfg_out);
    ospf_db_if_cfg_sanitize(cfg_out);
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int ospf_db_del_interface(uint32_t process_id, const char *ifname)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || process_id == 0u || !ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ospf_db_interface_pk(&pk, process_id, ifname);
    int rows = db_rpc_delete(ctx, OSPF_TABLE_INTERFACE, &pk.filter);
    db_filter_clear(&pk);
    return rows >= 0 ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int ospf_db_restore_instances(GHashTable *active_instances)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    db_result_t *result = NULL;
    if (!ctx || !active_instances)
    {
        return ERRCODE_FAIL;
    }
    if (db_rpc_query(ctx, OSPF_TABLE_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (!result)
    {
        return ERRCODE_SUCCESS;
    }

    int status = ERRCODE_SUCCESS;
    for (uint32_t i = 0u; i < result->num_rows; ++i)
    {
        db_row_t *row = result->rows[i];
        int64_t process_id_value = db_row_get_int(row, "process_id", 0);
        int64_t router_id_value = db_row_get_int(row, "router_id", 0);
        int64_t admin_up = db_row_get_int(row, "admin_up", 1);
        int64_t vrf_id_value = db_row_get_int(row, "vrf_id", VRF_PUBLIC_VRF_ID);
        const char *vrf_name = db_row_get_text(row, "vrf_name", VRF_PUBLIC_VRF_NAME);
        if (process_id_value <= 0 || !ospf_db_int_is_u32(process_id_value))
        {
            LOG_WARN("OSPF: skipping invalid persisted process id");
            continue;
        }
        if (!admin_up)
        {
            continue;
        }

        uint32_t process_id = (uint32_t)process_id_value;
        ospf_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = OSPF_APPLY_INSTANCE_SET;
        apply.u.instance_set.process_id = process_id;
        apply.u.instance_set.router_id = ospf_db_int_is_u32(router_id_value) ? (uint32_t)router_id_value : 0u;
        apply.u.instance_set.vrf_id = ospf_db_int_is_u32(vrf_id_value) ? (uint32_t)vrf_id_value : VRF_PUBLIC_VRF_ID;
        g_strlcpy(apply.u.instance_set.vrf_name, (vrf_name && vrf_name[0] != '\0') ? vrf_name : VRF_PUBLIC_VRF_NAME,
                  sizeof(apply.u.instance_set.vrf_name));

        if (ospf_worker_dispatch_apply(&apply) != ERRCODE_SUCCESS || apply.rc == OSPF_APPLY_RC_FAIL)
        {
            LOG_WARN("OSPF: failed to restore process %u", process_id);
            status = ERRCODE_FAIL;
            continue;
        }
        g_hash_table_add(active_instances, GUINT_TO_POINTER(process_id));
    }

    db_result_free(result);
    return status;
}

static guint64 ospf_db_area_key(uint32_t process_id, uint32_t area_id)
{
    return ((guint64)process_id << 32u) | area_id;
}

static gboolean ospf_db_active_area_contains(GHashTable *active_areas, uint32_t process_id, uint32_t area_id)
{
    guint64 key = ospf_db_area_key(process_id, area_id);
    return g_hash_table_contains(active_areas, &key);
}

static void ospf_db_active_area_add(GHashTable *active_areas, uint32_t process_id, uint32_t area_id)
{
    if (ospf_db_active_area_contains(active_areas, process_id, area_id))
    {
        return;
    }

    guint64 *key = g_new(guint64, 1);
    *key = ospf_db_area_key(process_id, area_id);
    g_hash_table_add(active_areas, key);
}

static int ospf_db_apply_restored_area(uint32_t process_id, uint32_t area_id, GHashTable *active_areas)
{
    if (ospf_db_active_area_contains(active_areas, process_id, area_id))
    {
        return ERRCODE_SUCCESS;
    }

    ospf_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = OSPF_APPLY_AREA_SET;
    apply.u.area_set.process_id = process_id;
    apply.u.area_set.area_id = area_id;
    if (ospf_worker_dispatch_apply(&apply) != ERRCODE_SUCCESS || apply.rc == OSPF_APPLY_RC_FAIL)
    {
        return ERRCODE_FAIL;
    }

    ospf_db_active_area_add(active_areas, process_id, area_id);
    return ERRCODE_SUCCESS;
}

static int ospf_db_restore_areas(GHashTable *active_instances, GHashTable *active_areas)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    db_result_t *result = NULL;
    if (!ctx || !active_instances || !active_areas)
    {
        return ERRCODE_FAIL;
    }
    if (db_rpc_query(ctx, OSPF_TABLE_AREA, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (!result)
    {
        return ERRCODE_SUCCESS;
    }

    int status = ERRCODE_SUCCESS;
    for (uint32_t i = 0u; i < result->num_rows; ++i)
    {
        db_row_t *row = result->rows[i];
        int64_t process_id_value = db_row_get_int(row, "process_id", 0);
        int64_t area_id_value = db_row_get_int(row, "area_id", -1);
        if (process_id_value <= 0 || !ospf_db_int_is_u32(process_id_value) || !ospf_db_int_is_u32(area_id_value))
        {
            LOG_WARN("OSPF: skipping invalid persisted area row");
            continue;
        }

        uint32_t process_id = (uint32_t)process_id_value;
        uint32_t area_id = (uint32_t)area_id_value;
        if (!g_hash_table_contains(active_instances, GUINT_TO_POINTER(process_id)))
        {
            continue;
        }
        if (ospf_db_apply_restored_area(process_id, area_id, active_areas) != ERRCODE_SUCCESS)
        {
            LOG_WARN("OSPF: failed to restore process %u area %u", process_id, area_id);
            status = ERRCODE_FAIL;
        }
    }

    db_result_free(result);
    return status;
}

static int ospf_db_restore_interface_area(uint32_t process_id, uint32_t area_id, GHashTable *active_areas)
{
    if (ospf_db_active_area_contains(active_areas, process_id, area_id))
    {
        return ERRCODE_SUCCESS;
    }

    if (ospf_db_set_area(process_id, area_id) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: failed to persist legacy process %u area %u", process_id, area_id);
        return ERRCODE_FAIL;
    }
    if (ospf_db_apply_restored_area(process_id, area_id, active_areas) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: failed to restore legacy process %u area %u", process_id, area_id);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

static int ospf_db_restore_interfaces(GHashTable *active_instances, GHashTable *active_areas)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    db_result_t *result = NULL;
    if (!ctx || !active_instances || !active_areas)
    {
        return ERRCODE_FAIL;
    }
    if (db_rpc_query(ctx, OSPF_TABLE_INTERFACE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (!result)
    {
        return ERRCODE_SUCCESS;
    }

    int status = ERRCODE_SUCCESS;
    for (uint32_t i = 0u; i < result->num_rows; ++i)
    {
        db_row_t *row = result->rows[i];
        int64_t process_id_value = db_row_get_int(row, "process_id", 0);
        const char *ifname = db_row_get_text(row, "ifname", NULL);
        if (process_id_value <= 0 || !ospf_db_int_is_u32(process_id_value) || !ifname || ifname[0] == '\0' ||
            strlen(ifname) >= sizeof(((ospf_if_cfg_t *)0)->ifname))
        {
            LOG_WARN("OSPF: skipping invalid persisted interface row");
            continue;
        }

        uint32_t process_id = (uint32_t)process_id_value;
        if (!g_hash_table_contains(active_instances, GUINT_TO_POINTER(process_id)))
        {
            continue;
        }

        ospf_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = OSPF_APPLY_IF_SET;
        apply.u.if_set.process_id = process_id;
        ospf_db_if_cfg_defaults(&apply.u.if_set.cfg, ifname);
        ospf_db_if_cfg_from_row(row, &apply.u.if_set.cfg);
        ospf_db_if_cfg_sanitize(&apply.u.if_set.cfg);

        if (ospf_db_restore_interface_area(process_id, apply.u.if_set.cfg.area_id, active_areas) != ERRCODE_SUCCESS)
        {
            status = ERRCODE_FAIL;
            continue;
        }

        if (ospf_worker_dispatch_apply(&apply) != ERRCODE_SUCCESS || apply.rc == OSPF_APPLY_RC_FAIL)
        {
            LOG_WARN("OSPF: failed to restore process %u interface %s", process_id, ifname);
            status = ERRCODE_FAIL;
        }
    }

    db_result_free(result);
    return status;
}

int ospf_db_restore(void)
{
    GHashTable *active_instances = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *active_areas = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    if (!active_instances || !active_areas)
    {
        if (active_instances)
        {
            g_hash_table_destroy(active_instances);
        }
        if (active_areas)
        {
            g_hash_table_destroy(active_areas);
        }
        return ERRCODE_FAIL;
    }

    int instance_rc = ospf_db_restore_instances(active_instances);
    int area_rc = ospf_db_restore_areas(active_instances, active_areas);
    int interface_rc = ospf_db_restore_interfaces(active_instances, active_areas);
    g_hash_table_destroy(active_areas);
    g_hash_table_destroy(active_instances);

    return (instance_rc == ERRCODE_SUCCESS && area_rc == ERRCODE_SUCCESS && interface_rc == ERRCODE_SUCCESS)
               ? ERRCODE_SUCCESS
               : ERRCODE_FAIL;
}
