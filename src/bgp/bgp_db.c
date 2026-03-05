/**
 * @file   bgp_db.c
 * @brief  BGP 模块数据库操作实现（封装 db_rpc 调用）
 * @author jhb
 * @date   2026/02/23
 */
#include "bgp_db.h"

#include <stdio.h>
#include <string.h>

#include "bgp_protocol.h"
#include "bgp_vrf.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 表定义
// ============================================================================

/* bgp_protocol 表列定义 */
static const db_column_def_t BGP_PROTOCOL_COLS[] = {
    {"as_number", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
    {"router_id", DB_TYPE_TEXT, DB_COL_NOT_NULL, "'0.0.0.0'"},
};

/* bgp_protocol 表定义 */
static const db_table_def_t BGP_PROTOCOL_TABLE = {
    .table_name = BGP_TABLE_PROTOCOL,
    .cols = BGP_PROTOCOL_COLS,
    .num_cols = G_N_ELEMENTS(BGP_PROTOCOL_COLS),
};

/* bgp_session 表列定义 */
static const db_column_def_t BGP_SESSION_COLS[] = {
    {"neighbor_ip", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"remote_as", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"vrf", DB_TYPE_TEXT, DB_COL_NOT_NULL, "'default'"},
};

/* bgp_session 表定义 */
static const db_table_def_t BGP_SESSION_TABLE = {
    .table_name = BGP_TABLE_SESSION,
    .cols = BGP_SESSION_COLS,
    .num_cols = G_N_ELEMENTS(BGP_SESSION_COLS),
};

/* bgp_neighbor 表列定义 */
static const db_column_def_t BGP_NEIGHBOR_COLS[] = {
    {"neighbor_ip", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"afi", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
};

/* bgp_neighbor 表定义 */
static const db_table_def_t BGP_NEIGHBOR_TABLE = {
    .table_name = BGP_TABLE_NEIGHBOR,
    .cols = BGP_NEIGHBOR_COLS,
    .num_cols = G_N_ELEMENTS(BGP_NEIGHBOR_COLS),
};

// ============================================================================
// 启动恢复 - 内部函数（按表拆分）
// ============================================================================

/**
 * @brief 从 bgp_protocol 表恢复协议对象
 * @return 恢复的 bgp_protocol_t 指针，无配置或失败返回 NULL
 */
static bgp_protocol_t *restore_protocol(dev_ipc_context_t *ctx)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_PROTOCOL, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return NULL;
    }

    if (result->num_rows == 0)
    {
        db_result_free(result);
        LOG_INFO("BGP 数据库无配置，跳过恢复");
        return NULL;
    }

    db_row_t *row = result->rows[0];
    uint32_t as_number = (uint32_t)db_row_get_int(row, "as_number", 0);
    const char *router_id = db_row_get_text(row, "router_id", NULL);

    bgp_protocol_t *proto = NULL;
    if (as_number != 0)
    {
        proto = bgp_protocol_create(as_number);
        if (router_id && strcmp(router_id, "0.0.0.0") != 0)
        {
            snprintf(proto->router_id, sizeof(proto->router_id), "%s", router_id);
        }
        LOG_INFO("BGP 协议已恢复: AS %u, router-id %s", proto->as_number, proto->router_id);
    }

    db_result_free(result);
    return proto;
}

/**
 * @brief 从 bgp_session 表恢复会话到 VRF
 * @param vrf0 目标 VRF（为 NULL 时直接返回）
 */
static void restore_sessions(dev_ipc_context_t *ctx, bgp_vrf_t *vrf0)
{
    if (!vrf0)
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

        if (!ip_val)
        {
            continue;
        }

        net_addr_t nb_addr;
        if (net_addr_from_str(ip_val, &nb_addr) != 0)
        {
            LOG_WARN("BGP 恢复: session 邻居地址 %s 解析失败，跳过", ip_val);
            continue;
        }
        bgp_session_t *sess = bgp_session_create(&nb_addr, as_val);
        bgp_vrf_add_session(vrf0, sess);
    }

    db_result_free(result);
}

/**
 * @brief 从 bgp_neighbor 表恢复地址族邻居到 VRF
 * @param vrf0 目标 VRF（为 NULL 时直接返回）
 */
static void restore_neighbors(dev_ipc_context_t *ctx, bgp_vrf_t *vrf0)
{
    if (!vrf0)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_NEIGHBOR, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *nb_ip = db_row_get_text(row, "neighbor_ip", NULL);
        const char *afi_str = db_row_get_text(row, "afi", NULL);

        if (!nb_ip || !afi_str)
        {
            continue;
        }

        /* 目前仅支持 "ipv4-unicast"，按需扩展 */
        bgp_afi_t afi = BGP_AFI_IPV4;
        bgp_safi_t safi = BGP_SAFI_UNICAST;

        net_addr_t nb_addr;
        if (net_addr_from_str(nb_ip, &nb_addr) != 0)
        {
            LOG_WARN("BGP 恢复: 邻居地址 %s 解析失败，跳过", nb_ip);
        }
        else if (bgp_vrf_af_enable_neighbor(vrf0, afi, safi, &nb_addr) != 0)
        {
            LOG_WARN("BGP 恢复: 邻居 %s AF %s 使能失败（session 可能不存在）", nb_ip, afi_str);
        }
        else
        {
            LOG_INFO("BGP 恢复: 邻居 %s AF %s 已恢复", nb_ip, afi_str);
        }
    }

    db_result_free(result);
}

// ============================================================================
// 启动恢复
// ============================================================================

bgp_protocol_t *bgp_db_restore(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return NULL;
    }

    bgp_protocol_t *proto = restore_protocol(ctx);
    if (!proto)
    {
        return NULL;
    }

    bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
    restore_sessions(ctx, vrf0);
    restore_neighbors(ctx, vrf0);
    return proto;
}

// ============================================================================
// 建表初始化
// ============================================================================

int bgp_db_init(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return -1;
    }

    int ret = db_rpc_create_table_from_def(ctx, &BGP_PROTOCOL_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 建表失败: %s", BGP_TABLE_PROTOCOL);
        return -1;
    }
    LOG_INFO("BGP 数据库表 %s 已就绪", BGP_TABLE_PROTOCOL);

    ret = db_rpc_create_table_from_def(ctx, &BGP_SESSION_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 建表失败: %s", BGP_TABLE_SESSION);
        return -1;
    }
    LOG_INFO("BGP 数据库表 %s 已就绪", BGP_TABLE_SESSION);

    ret = db_rpc_create_table_from_def(ctx, &BGP_NEIGHBOR_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 建表失败: %s", BGP_TABLE_NEIGHBOR);
        return -1;
    }
    LOG_INFO("BGP 数据库表 %s 已就绪", BGP_TABLE_NEIGHBOR);

    return 0;
}

// ============================================================================
// 写入（插入或更新）AS 号
// ============================================================================

int bgp_db_set_as(dev_ipc_context_t *ctx, uint32_t as_number)
{
    if (!ctx)
    {
        return -1;
    }

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "as_number", (int64_t)as_number);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_PROTOCOL, rec, NULL);
    db_record_free(rec);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 AS 号 %u 失败", as_number);
        return -1;
    }

    LOG_INFO("BGP AS 号 %u 已写入", as_number);
    return 0;
}

// ============================================================================
// 删除 AS 配置
// ============================================================================

int bgp_db_del_as(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return -1;
    }

    int rows = db_rpc_delete(ctx, BGP_TABLE_PROTOCOL, NULL);
    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 AS 配置失败");
        return -1;
    }

    LOG_INFO("BGP 删除 AS 配置，影响行数: %d", rows);
    return rows;
}

// ============================================================================
// BGP Session 操作
// ============================================================================

int bgp_db_set_session(dev_ipc_context_t *ctx, const char *vrf, const char *neighbor_ip, uint32_t remote_as)
{
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    const char *vrf_val = vrf ? vrf : BGP_VRF_PUBLIC_NAME;

    db_condition_t key_conditions[] = {
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
        {.field_name = "vrf", .op = DB_CMP_EQ, .value = db_value_text(vrf_val)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "neighbor_ip", neighbor_ip);
    db_record_set_int(rec, "remote_as", (int64_t)remote_as);
    db_record_set_text(rec, "vrf", vrf_val);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_SESSION, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 session vrf=%s neighbor=%s 失败", vrf_val, neighbor_ip);
        return -1;
    }

    LOG_INFO("BGP session vrf=%s neighbor=%s AS=%u 已写入", vrf_val, neighbor_ip, remote_as);
    return 0;
}

int bgp_db_del_session(dev_ipc_context_t *ctx, const char *vrf, const char *neighbor_ip)
{
    if (!ctx)
    {
        return -1;
    }

    int rows;

    if (!vrf && !neighbor_ip)
    {
        db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, NULL);
        rows = db_rpc_delete(ctx, BGP_TABLE_SESSION, NULL);
    }
    else
    {
        db_condition_t conditions[2];
        uint32_t num = 0;

        if (neighbor_ip)
        {
            conditions[num++] = (db_condition_t){
                .field_name = "neighbor_ip",
                .op = DB_CMP_EQ,
                .value = db_value_text(neighbor_ip),
            };
        }
        if (vrf)
        {
            conditions[num++] = (db_condition_t){
                .field_name = "vrf",
                .op = DB_CMP_EQ,
                .value = db_value_text(vrf),
            };
        }

        db_filter_t filter = {.conditions = conditions, .num_conditions = num};

        /* 同时清理 neighbor 记录 */
        if (neighbor_ip)
        {
            db_condition_t nb_cond = {
                .field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)};
            db_filter_t nb_filter = {.conditions = &nb_cond, .num_conditions = 1};
            db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &nb_filter);
            db_value_free(&nb_cond.value);
        }

        rows = db_rpc_delete(ctx, BGP_TABLE_SESSION, &filter);

        for (uint32_t i = 0; i < num; i++)
        {
            db_value_free(&conditions[i].value);
        }
    }

    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 session 失败");
        return -1;
    }

    LOG_INFO("BGP 删除 session（vrf=%s neighbor=%s），影响行数: %d", vrf ? vrf : "*", neighbor_ip ? neighbor_ip : "*",
             rows);
    return rows;
}

// ============================================================================
// BGP Neighbor 操作（地址族）
// ============================================================================

int bgp_db_set_neighbor(dev_ipc_context_t *ctx, const char *neighbor_ip, const char *afi)
{
    if (!ctx || !neighbor_ip || !afi)
    {
        return -1;
    }

    db_condition_t key_conditions[] = {
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_text(afi)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    /* neighbor 记录以 (neighbor_ip, afi) 为主键，已存在时无需更新，直接幂等插入 */
    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, BGP_TABLE_NEIGHBOR, &key_filter, &exists);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 查询 neighbor 存在性失败");
        return -1;
    }

    if (exists)
    {
        LOG_INFO("BGP neighbor %s afi %s 已存在", neighbor_ip, afi);
        return 0;
    }

    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "neighbor_ip", neighbor_ip);
    db_record_set_text(rec, "afi", afi);
    ret = db_rpc_insert_record(ctx, BGP_TABLE_NEIGHBOR, rec);
    db_record_free(rec);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 插入 neighbor %s afi %s 失败", neighbor_ip, afi);
        return -1;
    }

    LOG_INFO("BGP neighbor %s afi %s 已使能", neighbor_ip, afi);
    return 0;
}

int bgp_db_del_neighbor(dev_ipc_context_t *ctx, const char *neighbor_ip, const char *afi)
{
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    db_condition_t conditions[2];
    uint32_t num_conditions = 0;
    conditions[num_conditions++] = (db_condition_t){
        .field_name = "neighbor_ip",
        .op = DB_CMP_EQ,
        .value = db_value_text(neighbor_ip),
    };
    if (afi)
    {
        conditions[num_conditions++] = (db_condition_t){
            .field_name = "afi",
            .op = DB_CMP_EQ,
            .value = db_value_text(afi),
        };
    }
    db_filter_t filter = {.conditions = conditions, .num_conditions = num_conditions};

    int rows = db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &filter);
    db_value_free(&conditions[0].value);
    if (afi)
    {
        db_value_free(&conditions[1].value);
    }
    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 neighbor 失败");
        return -1;
    }

    LOG_INFO("BGP 删除 neighbor %s，影响行数: %d", neighbor_ip, rows);
    return rows;
}
