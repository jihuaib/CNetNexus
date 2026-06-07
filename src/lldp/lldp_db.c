/**
 * @file   lldp_db.c
 * @brief  LLDP 数据库协调层
 * @author jhb
 * @date   2026/06/07
 */
#include "lldp_db.h"

#include "db/lldp_db_internal.h"
#include "errcode.h"
#include "log.h"

int lldp_db_init(void)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    static const db_table_def_t *const LLDP_TABLES[] = {
        &LLDP_PROTO_TABLE,
        &LLDP_IF_TABLE,
    };

    for (size_t i = 0u; i < G_N_ELEMENTS(LLDP_TABLES); ++i)
    {
        int ret = db_rpc_create_table_from_def(ctx, LLDP_TABLES[i]);
        if (ret != ERRCODE_SUCCESS)
        {
            LOG_ERROR("LLDP: create table %s failed", LLDP_TABLES[i]->table_name);
            return ERRCODE_FAIL;
        }
        LOG_INFO("LLDP database table %s ready", LLDP_TABLES[i]->table_name);
    }

    return ERRCODE_SUCCESS;
}

int lldp_db_restore(void)
{
    if (!lldp_local_ipc_ctx())
    {
        return ERRCODE_FAIL;
    }

    lldp_db_restore_proto();
    lldp_db_restore_interfaces();
    return ERRCODE_SUCCESS;
}
