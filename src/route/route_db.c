/**
 * @file   route_db.c
 * @brief  Route 模块 DB 操作实现（在 IPC 线程调用）
 * @author jhb
 * @date   2026/03/28
 */
#include "route_db.h"

#include <stdio.h>
#include <string.h>

#include "db.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_cfg_apply.h"
#include "route_main.h"
#include "route_worker.h"

void route_db_upsert_static(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t afi, const char *prefix_str,
                            uint8_t prefix_len, const char *nexthop_str, int32_t metric, int32_t preference)
{
    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "vrf_id", vrf_id);
    db_record_set_int(rec, "afi", afi);
    db_record_set_text(rec, "prefix", prefix_str);
    db_record_set_int(rec, "prefix_len", prefix_len);
    db_record_set_text(rec, "nexthop", nexthop_str);
    db_record_set_int(rec, "metric", metric);
    db_record_set_int(rec, "preference", preference);

    db_condition_t conds[5];
    uint32_t nc = 0;
    conds[nc++] = (db_condition_t){"vrf_id", DB_CMP_EQ, db_value_int(vrf_id)};
    conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
    conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix_str)};
    conds[nc++] = (db_condition_t){"prefix_len", DB_CMP_EQ, db_value_int(prefix_len)};
    conds[nc++] = (db_condition_t){"nexthop", DB_CMP_EQ, db_value_text(nexthop_str)};
    db_filter_t filter = {conds, nc};

    db_rpc_upsert(ctx, "route_static", rec, &filter);
    db_record_free(rec);
    for (uint32_t i = 0; i < nc; i++)
    {
        db_value_free(&conds[i].value);
    }
}

void route_db_delete_static(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t afi, const char *prefix_str,
                            uint8_t prefix_len, const char *nexthop_str)
{
    db_condition_t conds[5];
    uint32_t nc = 0;
    conds[nc++] = (db_condition_t){"vrf_id", DB_CMP_EQ, db_value_int(vrf_id)};
    conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
    conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix_str)};
    conds[nc++] = (db_condition_t){"prefix_len", DB_CMP_EQ, db_value_int(prefix_len)};
    conds[nc++] = (db_condition_t){"nexthop", DB_CMP_EQ, db_value_text(nexthop_str)};
    db_filter_t filter = {conds, nc};

    db_rpc_delete(ctx, "route_static", &filter);
    for (uint32_t i = 0; i < nc; i++)
    {
        db_value_free(&conds[i].value);
    }
}

void route_db_delete_static_prefix(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t afi, const char *prefix_str,
                                   uint8_t prefix_len)
{
    db_condition_t conds[4];
    uint32_t nc = 0;
    conds[nc++] = (db_condition_t){"vrf_id", DB_CMP_EQ, db_value_int(vrf_id)};
    conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
    conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix_str)};
    conds[nc++] = (db_condition_t){"prefix_len", DB_CMP_EQ, db_value_int(prefix_len)};
    db_filter_t filter = {conds, nc};

    db_rpc_delete(ctx, "route_static", &filter);
    for (uint32_t i = 0; i < nc; i++)
    {
        db_value_free(&conds[i].value);
    }
}

void route_db_upsert_batch(dev_ipc_context_t *ctx, const char *name, uint16_t afi, const char *start_addr,
                           uint8_t prefix_len, int64_t count, const char *nexthop)
{
    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "name", name);
    db_record_set_int(rec, "afi", afi);
    db_record_set_text(rec, "start_addr", start_addr);
    db_record_set_int(rec, "prefix_len", prefix_len);
    db_record_set_int(rec, "count", count);
    db_record_set_text(rec, "nexthop", nexthop);

    db_condition_t cond = {"name", DB_CMP_EQ, db_value_text(name)};
    db_filter_t filter = {&cond, 1};

    db_rpc_upsert(ctx, "route_batch", rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);
}

void route_db_delete_batch(dev_ipc_context_t *ctx, const char *name)
{
    db_condition_t cond = {"name", DB_CMP_EQ, db_value_text(name)};
    db_filter_t filter = {&cond, 1};

    db_rpc_delete(ctx, "route_batch", &filter);
    db_value_free(&cond.value);
}

// ============================================================================
// 启动恢复（通过 worker 派发 apply，避免 IPC 线程直调 work 内部函数）
// ============================================================================

static int route_restore_static_from_db(dev_ipc_context_t *ctx)
{
    db_result_t *result = NULL;
    int ret = db_rpc_query(ctx, "route_static", NULL, 0, NULL, &result);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Route restore: query route_static failed (ret=%d)", ret);
        return ERRCODE_FAIL;
    }

    if (!result || result->num_rows == 0)
    {
        db_result_free(result);
        LOG_INFO("Route restore: no static routes in DB");
        return ERRCODE_SUCCESS;
    }

    uint32_t restored = 0;
    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", ROUTE_VRF_DEFAULT);
        uint16_t afi = (uint16_t)db_row_get_int(row, "afi", ROUTE_AFI_IPV4);
        uint8_t prefix_len = (uint8_t)db_row_get_int(row, "prefix_len", 0);
        int32_t metric = (int32_t)db_row_get_int(row, "metric", 0);
        int32_t preference = (int32_t)db_row_get_int(row, "preference", ROUTE_ADMIN_DIST_STATIC);
        const char *prefix = db_row_get_text(row, "prefix", NULL);
        const char *nexthop = db_row_get_text(row, "nexthop", NULL);

        if (!prefix || !nexthop)
        {
            continue;
        }

        net_addr_t prefix_addr;
        net_addr_t nexthop_addr;
        if (net_addr_from_str(prefix, &prefix_addr) != 0 || net_addr_from_str(nexthop, &nexthop_addr) != 0)
        {
            LOG_WARN("Route restore: invalid static row prefix='%s' nexthop='%s', skipped", prefix, nexthop);
            continue;
        }
        if (net_addr_prefix_normalize(&prefix_addr, prefix_len) != 0)
        {
            LOG_WARN("Route restore: invalid static prefix len row prefix='%s' len=%u, skipped", prefix, prefix_len);
            continue;
        }

        char normalized_prefix[64] = {0};
        net_addr_to_str(&prefix_addr, normalized_prefix, sizeof(normalized_prefix));
        if (strcmp(prefix, normalized_prefix) != 0)
        {
            /* 启动恢复时顺带迁移历史非规范前缀，避免后续删除匹配不到旧记录 */
            route_db_upsert_static(ctx, vrf_id, afi, normalized_prefix, prefix_len, nexthop, metric, preference);
            route_db_delete_static(ctx, vrf_id, afi, prefix, prefix_len, nexthop);
            LOG_INFO("Route restore: normalized static prefix %s/%u -> %s/%u", prefix, prefix_len, normalized_prefix,
                     prefix_len);
        }

        route_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = ROUTE_APPLY_STATIC_ADD;
        apply.u.static_add.vrf_id = vrf_id;
        apply.u.static_add.afi = afi;
        apply.u.static_add.prefix_len = prefix_len;
        apply.u.static_add.prefix_addr = prefix_addr;
        apply.u.static_add.nexthop_addr = nexthop_addr;
        apply.u.static_add.metric = metric;
        apply.u.static_add.preference = preference;

        if (route_worker_dispatch_apply(&apply) != 0 || apply.rc < 0)
        {
            LOG_WARN("Route restore: static apply failed vrf=%u afi=%u pfx=%s/%u nh=%s", vrf_id, afi, prefix,
                     prefix_len, nexthop);
            continue;
        }

        restored++;
    }

    LOG_INFO("Route restore: static restored %u/%u", restored, result->num_rows);
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

static int route_restore_batch_from_db(dev_ipc_context_t *ctx)
{
    db_result_t *result = NULL;
    int ret = db_rpc_query(ctx, "route_batch", NULL, 0, NULL, &result);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Route restore: query route_batch failed (ret=%d)", ret);
        return ERRCODE_FAIL;
    }

    if (!result || result->num_rows == 0)
    {
        db_result_free(result);
        LOG_INFO("Route restore: no batch routes in DB");
        return ERRCODE_SUCCESS;
    }

    uint32_t restored = 0;
    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *name = db_row_get_text(row, "name", NULL);
        const char *start_addr = db_row_get_text(row, "start_addr", NULL);
        const char *nexthop = db_row_get_text(row, "nexthop", NULL);
        uint16_t afi = (uint16_t)db_row_get_int(row, "afi", ROUTE_AFI_IPV4);
        uint8_t prefix_len = (uint8_t)db_row_get_int(row, "prefix_len", 0);
        int64_t count = db_row_get_int(row, "count", 0);

        if (!name || !start_addr || !nexthop || count <= 0)
        {
            continue;
        }

        route_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = ROUTE_APPLY_BATCH_ADD;
        snprintf(apply.u.batch_add.name, sizeof(apply.u.batch_add.name), "%s", name);
        apply.u.batch_add.afi = afi;
        apply.u.batch_add.prefix_len = prefix_len;
        snprintf(apply.u.batch_add.start_addr, sizeof(apply.u.batch_add.start_addr), "%s", start_addr);
        apply.u.batch_add.count = count;
        snprintf(apply.u.batch_add.nexthop, sizeof(apply.u.batch_add.nexthop), "%s", nexthop);

        if (route_worker_dispatch_apply(&apply) != 0 || apply.rc < 0)
        {
            LOG_WARN("Route restore: batch apply failed name='%s' afi=%u start=%s/%u count=%lld nh=%s", name, afi,
                     start_addr, prefix_len, (long long)count, nexthop);
            continue;
        }

        restored++;
    }

    LOG_INFO("Route restore: batch restored %u/%u groups", restored, result->num_rows);
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int route_db_restore(void)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    if (route_restore_static_from_db(ctx) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    if (route_restore_batch_from_db(ctx) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}
