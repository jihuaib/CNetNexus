/**
 * @file   snmp_db.c
 * @brief  SNMP module DB-backed configuration
 */
#include "snmp_db.h"

#include <string.h>

#include "errcode.h"
#include "log.h"
#include "snmp_main.h"

#define SNMP_CONFIG_PK_VALUE 1

static const db_column_def_t SNMP_CONFIG_COLS[] = {
    {"id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
    {"trap_host", DB_TYPE_TEXT, 0, NULL},
    {"trap_port", DB_TYPE_INTEGER, 0, NULL},
};

static const db_table_def_t SNMP_CONFIG_TABLE = {
    .table_name = SNMP_TABLE_CONFIG,
    .cols = SNMP_CONFIG_COLS,
    .num_cols = G_N_ELEMENTS(SNMP_CONFIG_COLS),
};

static db_filter_t make_pk_filter(db_condition_t *cond)
{
    cond->field_name = "id";
    cond->op = DB_CMP_EQ;
    cond->value = db_value_int(SNMP_CONFIG_PK_VALUE);

    db_filter_t filter = {.conditions = cond, .num_conditions = 1};
    return filter;
}

static void snmp_config_default(snmp_config_msg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->trap_port = SNMP_TRAP_DEFAULT_PORT;
}

int snmp_db_init(void)
{
    dev_ipc_context_t *ctx = snmp_local_ipc_ctx();
    if (db_rpc_create_table_from_def(ctx, &SNMP_CONFIG_TABLE) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SNMP: failed to create table %s", SNMP_TABLE_CONFIG);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int snmp_db_get_config(snmp_config_msg_t *cfg)
{
    if (!cfg)
    {
        return ERRCODE_FAIL;
    }
    snmp_config_default(cfg);

    dev_ipc_context_t *ctx = snmp_local_ipc_ctx();
    db_condition_t cond;
    db_filter_t filter = make_pk_filter(&cond);

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, SNMP_TABLE_CONFIG, NULL, 0, &filter, &result) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (!result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return 1;
    }

    const char *trap_host = db_row_get_text(result->rows[0], "trap_host", "");
    int64_t trap_port = db_row_get_int(result->rows[0], "trap_port", 0);
    if (trap_host && trap_host[0] != '\0' && trap_port > 0 && trap_port <= 65535)
    {
        cfg->trap_enabled = 1u;
        cfg->trap_port = (uint32_t)trap_port;
        g_strlcpy(cfg->trap_host, trap_host, sizeof(cfg->trap_host));
    }

    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int snmp_db_restore(void)
{
    snmp_config_msg_t cfg;
    int rc = snmp_db_get_config(&cfg);
    if (rc == ERRCODE_FAIL)
    {
        return ERRCODE_FAIL;
    }
    if (rc == 1)
    {
        LOG_INFO("SNMP: database has no trap config");
        return ERRCODE_SUCCESS;
    }

    snmp_agent_apply_config(&cfg);
    return ERRCODE_SUCCESS;
}

int snmp_db_set_config(const snmp_config_msg_t *cfg)
{
    if (!cfg)
    {
        return ERRCODE_FAIL;
    }

    const char *stored_host =
        (cfg->trap_enabled && cfg->trap_host[0] != '\0' && cfg->trap_port > 0 && cfg->trap_port <= 65535)
            ? cfg->trap_host
            : "";
    int64_t stored_port = stored_host[0] != '\0' ? (int64_t)cfg->trap_port : 0;

    dev_ipc_context_t *ctx = snmp_local_ipc_ctx();
    db_condition_t cond;
    db_filter_t filter = make_pk_filter(&cond);

    gboolean exists = FALSE;
    if (db_rpc_exists(ctx, SNMP_TABLE_CONFIG, &filter, &exists) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    if (exists)
    {
        db_col_t cols[] = {
            DB_COL_TEXT("trap_host", stored_host),
            DB_COL_INT("trap_port", stored_port),
        };
        int rows = db_rpc_update_cols(ctx, SNMP_TABLE_CONFIG, &filter, cols, G_N_ELEMENTS(cols));
        return rows < 0 ? ERRCODE_FAIL : ERRCODE_SUCCESS;
    }

    db_col_t cols[] = {
        DB_COL_INT("id", SNMP_CONFIG_PK_VALUE),
        DB_COL_TEXT("trap_host", stored_host),
        DB_COL_INT("trap_port", stored_port),
    };
    return db_rpc_insert_cols(ctx, SNMP_TABLE_CONFIG, cols, G_N_ELEMENTS(cols));
}

int snmp_db_disable_trap(void)
{
    snmp_config_msg_t cfg;
    snmp_config_default(&cfg);
    return snmp_db_set_config(&cfg);
}
