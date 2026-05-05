/**
 * @file   ldp_db.c
 * @brief  LDP 数据库协调层：建表、启动恢复
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_db.h"

#include "db/ldp_db_internal.h"
#include "errcode.h"
#include "log.h"

int ldp_db_init(void)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    static const db_table_def_t *const LDP_TABLES[] = {
        &LDP_PROTO_TABLE,
        &LDP_IF_TABLE,
    };

    for (size_t i = 0u; i < G_N_ELEMENTS(LDP_TABLES); ++i)
    {
        int ret = db_rpc_create_table_from_def(ctx, LDP_TABLES[i]);
        if (ret != ERRCODE_SUCCESS)
        {
            LOG_ERROR("LDP: create table %s failed", LDP_TABLES[i]->table_name);
            return ERRCODE_FAIL;
        }
        LOG_INFO("LDP database table %s ready", LDP_TABLES[i]->table_name);
    }

    return ERRCODE_SUCCESS;
}

int ldp_db_restore(void)
{
    if (!ldp_local_ipc_ctx())
    {
        return ERRCODE_FAIL;
    }

    ldp_db_restore_proto();
    ldp_db_restore_interfaces();
    return ERRCODE_SUCCESS;
}
