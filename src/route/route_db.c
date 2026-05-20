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
#include "pending.h"
#include "route.h"
#include "route_cfg_apply.h"
#include "route_main.h"
#include "route_worker.h"
#include "vrf.h"

/* 静态路由恢复挂起载荷：等待 VRF cache 就绪后重新应用 */
typedef struct route_static_pending_row
{
    char vrf_name[VRF_NAME_MAX_LEN];
    uint16_t afi;
    uint8_t prefix_len;
    int32_t metric;
    int32_t preference;
    char prefix[64];
    char nexthop[64];
    char ifname[IF_LOGICAL_NAME_MAX];
} route_static_pending_row_t;

static const char *route_db_safe_vrf_name(const char *vrf_name)
{
    if (!vrf_name || vrf_name[0] == '\0')
    {
        return VRF_PUBLIC_VRF_NAME;
    }
    return vrf_name;
}

void route_db_upsert_static(dev_ipc_context_t *ctx, const char *vrf_name, uint16_t afi, const char *prefix_str,
                            uint8_t prefix_len, const char *nexthop_str, int32_t metric, int32_t preference,
                            const char *ifname)
{
    const char *safe_ifname = ifname ? ifname : "";
    const char *safe_vrf = route_db_safe_vrf_name(vrf_name);

    /* PK：6 列联合（vrf_name+afi+prefix+len+nh+ifname）；可变列：metric/preference */
    db_filter_builder_t pk;
    db_filter_init(&pk);
    db_filter_add_text(&pk, "vrf_name", safe_vrf);
    db_filter_add_int(&pk, "afi", (int64_t)afi);
    db_filter_add_text(&pk, "prefix", prefix_str);
    db_filter_add_int(&pk, "prefix_len", (int64_t)prefix_len);
    db_filter_add_text(&pk, "nexthop", nexthop_str);
    db_filter_add_text(&pk, "ifname", safe_ifname);

    gboolean exists = FALSE;
    if (db_rpc_exists(ctx, "route_static", &pk.filter, &exists) != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        return;
    }

    if (exists)
    {
        db_col_t cols[] = {
            DB_COL_INT("metric", metric),
            DB_COL_INT("preference", preference),
        };
        (void)db_rpc_update_cols(ctx, "route_static", &pk.filter, cols, G_N_ELEMENTS(cols));
        db_filter_clear(&pk);
        return;
    }
    db_filter_clear(&pk);

    db_col_t cols[] = {
        DB_COL_TEXT("vrf_name", safe_vrf),    DB_COL_INT("afi", afi),
        DB_COL_TEXT("prefix", prefix_str),    DB_COL_INT("prefix_len", prefix_len),
        DB_COL_TEXT("nexthop", nexthop_str),  DB_COL_INT("metric", metric),
        DB_COL_INT("preference", preference), DB_COL_TEXT("ifname", safe_ifname),
    };
    (void)db_rpc_insert_cols(ctx, "route_static", cols, G_N_ELEMENTS(cols));
}

void route_db_delete_static(dev_ipc_context_t *ctx, const char *vrf_name, uint16_t afi, const char *prefix_str,
                            uint8_t prefix_len, const char *nexthop_str, const char *ifname)
{
    const char *safe_ifname = ifname ? ifname : "";
    const char *safe_vrf = route_db_safe_vrf_name(vrf_name);

    db_condition_t conds[6];
    uint32_t nc = 0;
    conds[nc++] = (db_condition_t){"vrf_name", DB_CMP_EQ, db_value_text(safe_vrf)};
    conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
    conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix_str)};
    conds[nc++] = (db_condition_t){"prefix_len", DB_CMP_EQ, db_value_int(prefix_len)};
    conds[nc++] = (db_condition_t){"nexthop", DB_CMP_EQ, db_value_text(nexthop_str)};
    conds[nc++] = (db_condition_t){"ifname", DB_CMP_EQ, db_value_text(safe_ifname)};
    db_filter_t filter = {conds, nc};

    db_rpc_delete(ctx, "route_static", &filter);
    for (uint32_t i = 0; i < nc; i++)
    {
        db_value_free(&conds[i].value);
    }
}

void route_db_delete_static_prefix(dev_ipc_context_t *ctx, const char *vrf_name, uint16_t afi, const char *prefix_str,
                                   uint8_t prefix_len)
{
    const char *safe_vrf = route_db_safe_vrf_name(vrf_name);

    db_condition_t conds[4];
    uint32_t nc = 0;
    conds[nc++] = (db_condition_t){"vrf_name", DB_CMP_EQ, db_value_text(safe_vrf)};
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

int route_db_delete_static_by_vrf(dev_ipc_context_t *ctx, const char *vrf_name)
{
    if (!ctx || !vrf_name || vrf_name[0] == '\0')
    {
        return -1;
    }

    db_condition_t cond = {"vrf_name", DB_CMP_EQ, db_value_text(vrf_name)};
    db_filter_t filter = {&cond, 1};

    int rc = db_rpc_delete(ctx, "route_static", &filter);
    db_value_free(&cond.value);
    return (rc == ERRCODE_SUCCESS) ? 0 : -1;
}

void route_db_upsert_batch(dev_ipc_context_t *ctx, const char *name, uint16_t afi, const char *start_addr,
                           uint8_t prefix_len, int64_t count, const char *nexthop)
{
    /* PK：name；可变列：afi/start_addr/prefix_len/count/nexthop */
    db_filter_builder_t pk;
    db_filter_init(&pk);
    db_filter_add_text(&pk, "name", name);

    gboolean exists = FALSE;
    if (db_rpc_exists(ctx, "route_batch", &pk.filter, &exists) != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        return;
    }

    if (exists)
    {
        db_col_t cols[] = {
            DB_COL_INT("afi", afi),     DB_COL_TEXT("start_addr", start_addr), DB_COL_INT("prefix_len", prefix_len),
            DB_COL_INT("count", count), DB_COL_TEXT("nexthop", nexthop),
        };
        (void)db_rpc_update_cols(ctx, "route_batch", &pk.filter, cols, G_N_ELEMENTS(cols));
        db_filter_clear(&pk);
        return;
    }
    db_filter_clear(&pk);

    db_col_t cols[] = {
        DB_COL_TEXT("name", name),
        DB_COL_INT("afi", afi),
        DB_COL_TEXT("start_addr", start_addr),
        DB_COL_INT("prefix_len", prefix_len),
        DB_COL_INT("count", count),
        DB_COL_TEXT("nexthop", nexthop),
    };
    (void)db_rpc_insert_cols(ctx, "route_batch", cols, G_N_ELEMENTS(cols));
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

static int route_db_apply_static_row(void *item, void *ctx_unused)
{
    (void)ctx_unused;
    const route_static_pending_row_t *r = (const route_static_pending_row_t *)item;
    if (!r)
    {
        return PENDING_DONE;
    }

    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    if (!ctx)
    {
        return PENDING_DONE;
    }

    /* 通过 worker 查询 VRF cache；未就绪时挂起，等 VRF_ADD 事件解锁 */
    uint32_t vrf_id = ROUTE_VRF_DEFAULT;
    int rc = route_worker_resolve_vrf_id_by_name(r->vrf_name, &vrf_id);
    if (rc != ERRCODE_SUCCESS)
    {
        LOG_INFO("Route restore: static '%s/%u' waits on vrf '%s', parked", r->prefix, r->prefix_len, r->vrf_name);
        pending_park(g_route_local->pending, ROUTE_DEP_VRF, g_str_hash(r->vrf_name), r, sizeof(*r), NULL,
                     route_db_apply_static_row, NULL);
        return PENDING_AGAIN;
    }

    /* nexthop 可以为空（interface-only 路由） */
    int has_nh = (r->nexthop[0] != '\0');

    net_addr_t prefix_addr;
    net_addr_t nexthop_addr;
    memset(&nexthop_addr, 0, sizeof(nexthop_addr));
    if (net_addr_from_str(r->prefix, &prefix_addr) != 0)
    {
        LOG_WARN("Route restore: invalid static row prefix='%s', skipped", r->prefix);
        return PENDING_DONE;
    }
    if (has_nh && net_addr_from_str(r->nexthop, &nexthop_addr) != 0)
    {
        LOG_WARN("Route restore: invalid static row nexthop='%s', skipped", r->nexthop);
        return PENDING_DONE;
    }
    if (!has_nh)
    {
        nexthop_addr.family = prefix_addr.family;
    }
    if (net_addr_prefix_normalize(&prefix_addr, r->prefix_len) != 0)
    {
        LOG_WARN("Route restore: invalid static prefix len row prefix='%s' len=%u, skipped", r->prefix, r->prefix_len);
        return PENDING_DONE;
    }

    char normalized_prefix[64] = {0};
    net_addr_to_str(&prefix_addr, normalized_prefix, sizeof(normalized_prefix));
    if (strcmp(r->prefix, normalized_prefix) != 0)
    {
        /* 启动恢复时顺带迁移历史非规范前缀，避免后续删除匹配不到旧记录 */
        route_db_upsert_static(ctx, r->vrf_name, r->afi, normalized_prefix, r->prefix_len, has_nh ? r->nexthop : "",
                               r->metric, r->preference, r->ifname);
        route_db_delete_static(ctx, r->vrf_name, r->afi, r->prefix, r->prefix_len, has_nh ? r->nexthop : "", r->ifname);
        LOG_INFO("Route restore: normalized static prefix %s/%u -> %s/%u", r->prefix, r->prefix_len, normalized_prefix,
                 r->prefix_len);
    }

    route_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = ROUTE_APPLY_STATIC_ADD;
    apply.u.static_add.vrf_id = vrf_id;
    apply.u.static_add.afi = r->afi;
    apply.u.static_add.prefix_len = r->prefix_len;
    apply.u.static_add.prefix_addr = prefix_addr;
    apply.u.static_add.nexthop_addr = nexthop_addr;
    apply.u.static_add.metric = r->metric;
    apply.u.static_add.preference = r->preference;
    g_strlcpy(apply.u.static_add.out_ifname, r->ifname, sizeof(apply.u.static_add.out_ifname));

    if (route_worker_dispatch_apply(&apply) != 0 || apply.rc < 0)
    {
        LOG_WARN("Route restore: static apply failed vrf=%s afi=%u pfx=%s/%u nh=%s", r->vrf_name, r->afi, r->prefix,
                 r->prefix_len, r->nexthop);
        return PENDING_DONE;
    }

    return PENDING_DONE;
}

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

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *prefix = db_row_get_text(row, "prefix", NULL);
        if (!prefix)
        {
            continue;
        }

        route_static_pending_row_t snap;
        memset(&snap, 0, sizeof(snap));
        g_strlcpy(snap.vrf_name, db_row_get_text(row, "vrf_name", VRF_PUBLIC_VRF_NAME), sizeof(snap.vrf_name));
        snap.afi = (uint16_t)db_row_get_int(row, "afi", ROUTE_AFI_IPV4);
        snap.prefix_len = (uint8_t)db_row_get_int(row, "prefix_len", 0);
        snap.metric = (int32_t)db_row_get_int(row, "metric", 0);
        snap.preference = (int32_t)db_row_get_int(row, "preference", ROUTE_ADMIN_DIST_STATIC);
        g_strlcpy(snap.prefix, prefix, sizeof(snap.prefix));
        g_strlcpy(snap.nexthop, db_row_get_text(row, "nexthop", ""), sizeof(snap.nexthop));
        g_strlcpy(snap.ifname, db_row_get_text(row, "ifname", ""), sizeof(snap.ifname));

        /* 乐观应用：缺 VRF 时 apply_row 内部自挂起 */
        (void)route_db_apply_static_row(&snap, NULL);
    }

    LOG_INFO("Route restore: static processed %u row(s), pending=%zu", result->num_rows,
             pending_count(g_route_local->pending));
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
