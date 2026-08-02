/**
 * @file   isis_db.c
 * @brief  ISIS 数据库协调层：建表、启动恢复
 *
 * 各表 schema 与 CRUD/restore 实现位于 src/isis/db/isis_db_<table>.c。
 * 本文件只负责把建表与恢复入口串起来。
 *
 * @author jhb
 * @date   2026/04/26
 */

#include "isis_db.h"

#include "db/isis_db_internal.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 建表初始化
// ============================================================================

int isis_db_init(void)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    static const db_table_def_t *const ISIS_TABLES[] = {
        &ISIS_INSTANCE_TABLE,
        &ISIS_IF_TABLE,
    };

    for (size_t i = 0u; i < G_N_ELEMENTS(ISIS_TABLES); ++i)
    {
        int ret = db_rpc_create_table_from_def(ctx, ISIS_TABLES[i]);
        if (ret != ERRCODE_SUCCESS)
        {
            LOG_ERROR("ISIS: create table %s failed", ISIS_TABLES[i]->table_name);
            return ERRCODE_FAIL;
        }
        LOG_INFO("ISIS database table %s ready", ISIS_TABLES[i]->table_name);
    }

    return ERRCODE_SUCCESS;
}

// ============================================================================
// 启动恢复
// ============================================================================

int isis_db_restore(void)
{
    if (!isis_local_ipc_ctx())
    {
        return ERRCODE_FAIL;
    }

    int instance_rc = isis_db_restore_instances();
    /* 接口恢复沿用原有独立、尽力而为语义；instance/IPv6 AF/locator 的
     * 错误仍通过最终返回值上送，避免牵连无关配置的 cold restore。 */
    isis_db_restore_interfaces();
    return instance_rc;
}
