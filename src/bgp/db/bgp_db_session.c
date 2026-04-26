/**
 * @file   bgp_db_session.c
 * @brief  BGP session 表：schema、CRUD、字段级 setter、启动恢复
 * @author jhb
 * @date   2026/04/26
 */

#include <stdio.h>
#include <string.h>

#include "bgp_db_internal.h"
#include "bgp_main.h"
#include "bgp_session.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 表 schema
// ============================================================================

static const db_column_def_t BGP_SESSION_COLS[] = {
    {"neighbor_ip", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"remote_as", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"open_caps", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "3"}, /* 默认 3 = AS4(bit0) + Route-Refresh(bit1) */
    {"source_interface", DB_TYPE_TEXT, DB_COL_NONE, NULL},
    {"ebgp_multihop", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
};

const db_table_def_t BGP_SESSION_TABLE = {
    .table_name = BGP_TABLE_SESSION,
    .cols = BGP_SESSION_COLS,
    .num_cols = G_N_ELEMENTS(BGP_SESSION_COLS),
};

// ============================================================================
// 整行 upsert / 删除
// ============================================================================

int bgp_db_set_session(uint32_t vrf_id, const char *neighbor_ip, uint32_t remote_as)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    /* 首次创建邻居：INSERT 整行 PK 列；其它列由表 schema 默认值兜底 */
    db_col_t cols[] = {
        DB_COL_TEXT("neighbor_ip", neighbor_ip),
        DB_COL_INT("vrf_id", vrf_id),
        DB_COL_INT("remote_as", remote_as),
    };
    int ret = db_rpc_insert_cols(ctx, BGP_TABLE_SESSION, cols, G_N_ELEMENTS(cols));
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP failed to write session vrf_id=%u neighbor=%s", vrf_id, neighbor_ip);
        return -1;
    }

    LOG_INFO("BGP session vrf_id=%u neighbor=%s AS=%u written", vrf_id, neighbor_ip, remote_as);
    return 0;
}

int bgp_db_del_session(uint32_t vrf_id, const char *neighbor_ip)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_filter_builder_t pk_session;
    db_filter_builder_t pk_neighbor;
    int rows = 0;
    int rows_nb = 0;

    if (!neighbor_ip)
    {
        /* 删除 vrf 内所有 session，并级联删除该 vrf 下所有 AF neighbor 记录 */
        bgp_db_vrf_pk(&pk_neighbor, vrf_id);
        rows_nb = db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &pk_neighbor.filter);
        rows = db_rpc_delete(ctx, BGP_TABLE_SESSION, &pk_neighbor.filter);
        db_filter_clear(&pk_neighbor);
    }
    else
    {
        /* PK 完全相同（vrf_id + neighbor_ip），neighbor 表多了 afi/safi，但不带这两个字段
         * 时 SQL 层退化为前缀删除——所以两张表共用 session_pk 即可。 */
        bgp_db_session_pk(&pk_neighbor, vrf_id, neighbor_ip);
        rows_nb = db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &pk_neighbor.filter);
        db_filter_clear(&pk_neighbor);

        bgp_db_session_pk(&pk_session, vrf_id, neighbor_ip);
        rows = db_rpc_delete(ctx, BGP_TABLE_SESSION, &pk_session.filter);
        db_filter_clear(&pk_session);
    }

    if (rows_nb < 0 || rows < 0)
    {
        LOG_ERROR("BGP failed to delete session/neighbor vrf_id=%u neighbor=%s", vrf_id,
                  neighbor_ip ? neighbor_ip : "*");
        return -1;
    }

    LOG_INFO("BGP deleted session/neighbor (vrf_id=%u neighbor=%s), session_rows=%d neighbor_rows=%d", vrf_id,
             neighbor_ip ? neighbor_ip : "*", rows, rows_nb);
    return rows;
}

// ============================================================================
// 字段级 setter
// ============================================================================

int bgp_db_set_session_caps(uint32_t vrf_id, const char *neighbor_ip, uint32_t open_caps)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_session_pk(&pk, vrf_id, neighbor_ip);

    db_col_t cols[] = {
        DB_COL_INT("open_caps", open_caps),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_SESSION, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write session open_caps vrf_id=%u neighbor=%s", vrf_id, neighbor_ip);
        return -1;
    }

    LOG_INFO("BGP session neighbor=%s open_caps=0x%02X written", neighbor_ip, open_caps);
    return 0;
}

int bgp_db_set_session_source_if(uint32_t vrf_id, const char *neighbor_ip, const char *if_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !neighbor_ip || !if_name)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_session_pk(&pk, vrf_id, neighbor_ip);

    db_col_t cols[] = {DB_COL_TEXT("source_interface", if_name)};
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_SESSION, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write session source-interface vrf_id=%u neighbor=%s if=%s", vrf_id, neighbor_ip,
                  if_name);
        return -1;
    }

    LOG_INFO("BGP session source-interface vrf_id=%u neighbor=%s if=%s written", vrf_id, neighbor_ip, if_name);
    return 0;
}

int bgp_db_del_session_source_if(uint32_t vrf_id, const char *neighbor_ip)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_session_pk(&pk, vrf_id, neighbor_ip);

    db_col_t cols[] = {DB_COL_TEXT("source_interface", "")};
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_SESSION, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to clear session source-interface vrf_id=%u neighbor=%s", vrf_id, neighbor_ip);
        return -1;
    }

    LOG_INFO("BGP session source-interface vrf_id=%u neighbor=%s cleared", vrf_id, neighbor_ip);
    return 0;
}

int bgp_db_set_session_ebgp_multihop(uint32_t vrf_id, const char *neighbor_ip, uint8_t ttl)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !neighbor_ip || ttl == 0)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_session_pk(&pk, vrf_id, neighbor_ip);

    db_col_t cols[] = {DB_COL_INT("ebgp_multihop", ttl)};
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_SESSION, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write session ebgp-multihop vrf_id=%u neighbor=%s ttl=%u", vrf_id, neighbor_ip, ttl);
        return -1;
    }

    LOG_INFO("BGP session ebgp-multihop vrf_id=%u neighbor=%s ttl=%u written", vrf_id, neighbor_ip, ttl);
    return 0;
}

int bgp_db_del_session_ebgp_multihop(uint32_t vrf_id, const char *neighbor_ip)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_session_pk(&pk, vrf_id, neighbor_ip);

    db_col_t cols[] = {DB_COL_INT("ebgp_multihop", 0)};
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_SESSION, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to clear session ebgp-multihop vrf_id=%u neighbor=%s", vrf_id, neighbor_ip);
        return -1;
    }

    LOG_INFO("BGP session ebgp-multihop vrf_id=%u neighbor=%s cleared", vrf_id, neighbor_ip);
    return 0;
}

// ============================================================================
// 启动恢复
// ============================================================================

void bgp_db_restore_sessions(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_SESSION, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *ip_val = db_row_get_text(row, "neighbor_ip", NULL);
        uint32_t as_val = (uint32_t)db_row_get_int(row, "remote_as", 0);
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);
        uint32_t open_caps = (uint32_t)db_row_get_int(row, "open_caps", (int64_t)BGP_SESS_CAP_DEFAULT);
        const char *source_if = db_row_get_text(row, "source_interface", "");
        uint32_t ebgp_multihop = (uint32_t)db_row_get_int(row, "ebgp_multihop", 0);

        if (!ip_val)
        {
            continue;
        }

        net_addr_t nb_addr;
        if (net_addr_from_str(ip_val, &nb_addr) != 0)
        {
            LOG_WARN("BGP restore: Session neighbor address %s parse failed, skipping", ip_val);
            continue;
        }

        /* 恢复邻居会话 */
        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_NEIGHBOR;
        apply.isNo = false;
        apply.vrf_id = vrf_id;
        apply.u.neighbor.addr = nb_addr;
        apply.u.neighbor.remote_as = as_val;
        if (bgp_worker_dispatch_apply(&apply) != 0 || apply.rc != BGP_APPLY_RC_OK)
        {
            LOG_WARN("BGP restore: Session neighbor %s creation failed, skipping", ip_val);
            continue;
        }

        /* 恢复能力位（按位逐一派发） */
        uint32_t cap_bits[] = {BGP_SESS_CAP_AS4, BGP_SESS_CAP_ROUTE_REFRESH};
        for (size_t c = 0; c < G_N_ELEMENTS(cap_bits); c++)
        {
            if (!(open_caps & cap_bits[c]))
            {
                continue; /* 能力未开启，跳过（默认关闭） */
            }
            bgp_apply_cmd_t cap_apply;
            memset(&cap_apply, 0, sizeof(cap_apply));
            cap_apply.group_id = BGP_CLI_GROUP_ID_OPEN_CAP;
            cap_apply.isNo = false;
            cap_apply.vrf_id = vrf_id;
            cap_apply.u.open_cap.addr = nb_addr;
            cap_apply.u.open_cap.cap_bit = cap_bits[c];
            (void)bgp_worker_dispatch_apply(&cap_apply);
        }

        if (source_if && source_if[0] != '\0')
        {
            bgp_apply_cmd_t src_if_apply;
            memset(&src_if_apply, 0, sizeof(src_if_apply));
            src_if_apply.group_id = BGP_CLI_GROUP_ID_SOURCE_IF;
            src_if_apply.isNo = false;
            src_if_apply.vrf_id = vrf_id;
            src_if_apply.u.source_if.addr = nb_addr;
            snprintf(src_if_apply.u.source_if.if_name, sizeof(src_if_apply.u.source_if.if_name), "%s", source_if);
            if (bgp_worker_dispatch_apply(&src_if_apply) != 0 || src_if_apply.rc != BGP_APPLY_RC_OK)
            {
                LOG_WARN("BGP restore: neighbor %s source-interface %s apply failed", ip_val, source_if);
            }
        }

        if (ebgp_multihop > 255)
        {
            LOG_WARN("BGP restore: neighbor %s ebgp-multihop %u out of range, skipping", ip_val, ebgp_multihop);
        }
        else if (ebgp_multihop > 0)
        {
            bgp_apply_cmd_t mh_apply;
            memset(&mh_apply, 0, sizeof(mh_apply));
            mh_apply.group_id = BGP_CLI_GROUP_ID_EBGP_MULTIHOP;
            mh_apply.isNo = false;
            mh_apply.vrf_id = vrf_id;
            mh_apply.u.ebgp_multihop.addr = nb_addr;
            mh_apply.u.ebgp_multihop.ttl = (uint8_t)(ebgp_multihop & 0xFFu);
            if (bgp_worker_dispatch_apply(&mh_apply) != 0 || mh_apply.rc != BGP_APPLY_RC_OK)
            {
                LOG_WARN("BGP restore: neighbor %s ebgp-multihop %u apply failed", ip_val, ebgp_multihop);
            }
        }
    }

    db_result_free(result);
}
