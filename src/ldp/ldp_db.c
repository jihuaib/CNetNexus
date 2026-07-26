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

    /*
     * DEV 会根据 protocol/interface 任一表在模块启动前决定是否 revive。
     * 模块一旦由旧脏 marker 或孤立子表拉起，建表后立即归一化，不再等待
     * IF smooth-end 的 restore。
     */
    if (ldp_db_sync_revive_marker(ctx) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: failed to normalize revive marker");
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int ldp_db_restore(void)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    if (ldp_db_sync_revive_marker(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: failed to reconcile revive marker before restore");
        return ERRCODE_FAIL;
    }
    ldp_db_restore_proto();
    ldp_db_restore_interfaces();
    return ERRCODE_SUCCESS;
}

int ldp_db_delete_config(void)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    /*
     * ldp_protocol 是 canonical revive marker，必须最后删除。这样并发的配置
     * 捕获/整机保存只会看到两种安全状态：
     *   1. marker 仍在，LDP 必须在线并参与捕获；
     *   2. 所有 LDP 配置已清空，marker 不在，可安全跳过 LDP。
     */
    int interface_rows = db_rpc_delete(ctx, LDP_TABLE_INTERFACE, NULL);
    if (interface_rows < 0)
    {
        LOG_ERROR("LDP: failed to clear interface configuration");
        return ERRCODE_FAIL;
    }

    int protocol_rows = db_rpc_delete(ctx, LDP_TABLE_PROTOCOL, NULL);
    if (protocol_rows < 0)
    {
        LOG_ERROR("LDP: failed to clear protocol revive marker");
        return ERRCODE_FAIL;
    }

    LOG_INFO("LDP configuration cleanup complete: protocol=%d interface=%d total=%d", protocol_rows, interface_rows,
             protocol_rows + interface_rows);
    return ERRCODE_SUCCESS;
}
