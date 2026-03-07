/**
 * @file   bgp_db.c
 * @brief  BGP 模块数据库操作实现（封装 db_rpc 调用）
 * @author jhb
 * @date   2026/02/23
 */
#include "bgp_db.h"

#include <stdio.h>
#include <string.h>

#include "bgp_cfg_apply.h"
#include "bgp_main.h"
#include "bgp_protocol.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 表定义
// ============================================================================

/* bgp_protocol 表列定义 */
static const db_column_def_t BGP_PROTOCOL_COLS[] = {{"as_number", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL}};

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
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"open_caps", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "3"}, /* 默认值 3 = AS4(bit0) + Route-Refresh(bit1) */
};

/* bgp_session 表定义 */
static const db_table_def_t BGP_SESSION_TABLE = {
    .table_name = BGP_TABLE_SESSION,
    .cols = BGP_SESSION_COLS,
    .num_cols = G_N_ELEMENTS(BGP_SESSION_COLS),
};

/* bgp_neighbor 表列定义 */
static const db_column_def_t BGP_NEIGHBOR_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"safi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"neighbor_ip", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
};

/* bgp_neighbor 表定义 */
static const db_table_def_t BGP_NEIGHBOR_TABLE = {
    .table_name = BGP_TABLE_NEIGHBOR,
    .cols = BGP_NEIGHBOR_COLS,
    .num_cols = G_N_ELEMENTS(BGP_NEIGHBOR_COLS),
};

/* bgp_instance 表列定义（vrf_id + afi + safi 三列联合唯一，无单列主键） */
static const db_column_def_t BGP_INSTANCE_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"safi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
};

/* bgp_instance 表定义 */
static const db_table_def_t BGP_INSTANCE_TABLE = {
    .table_name = BGP_TABLE_INSTANCE,
    .cols = BGP_INSTANCE_COLS,
    .num_cols = G_N_ELEMENTS(BGP_INSTANCE_COLS),
};

/* bgp_vrf 表列定义 */
static const db_column_def_t BGP_VRF_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},      {"router_id", DB_TYPE_TEXT, DB_COL_NOT_NULL, "0.0.0.0"},
    {"keepalive", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "60"},      {"hold_time", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "180"},
    {"connect_retry", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "120"},
};

/* bgp_vrf 表定义 */
static const db_table_def_t BGP_VRF_TABLE = {
    .table_name = BGP_TABLE_VRF,
    .cols = BGP_VRF_COLS,
    .num_cols = G_N_ELEMENTS(BGP_VRF_COLS),
};

/* 前向声明（write_defaults 中调用） */
int bgp_db_set_vrf_timers(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t keepalive, uint16_t hold_time);
int bgp_db_set_vrf_connect_retry(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t connect_retry);

// ============================================================================
// 启动恢复 - 内部函数（按表拆分）
// ============================================================================

/**
 * @brief 从 bgp_protocol 表恢复协议对象
 * @return 恢复的 bgp_protocol_t 指针，无配置或失败返回 NULL
 */
static uint32_t restore_protocol(dev_ipc_context_t *ctx)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_PROTOCOL, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return ERRCODE_FAIL;
    }

    if (result->num_rows == 0)
    {
        db_result_free(result);
        LOG_INFO("BGP 数据库无配置，跳过恢复");
        return ERRCODE_SUCCESS;
    }

    db_row_t *row = result->rows[0];
    uint32_t as_number = (uint32_t)db_row_get_int(row, "as_number", 0);

    if (as_number != 0)
    {
        uint32_t apply_ret = bgp_cfg_apply_protocol(FALSE, as_number);
        if (apply_ret == ERRCODE_SUCCESS)
        {
            LOG_INFO("BGP 协议已恢复: AS %u", as_number);
        }
        else
        {
            LOG_ERROR("BGP 恢复: 协议创建失败 (as=%u, ret=%d)", as_number, (int)apply_ret);
        }
    }

    db_result_free(result);
    return ERRCODE_SUCCESS;
}

static void restore_sessions(dev_ipc_context_t *ctx)
{
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

        bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, vrf_id);
        if (!vrf)
        {
            continue;
        }

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

        bgp_cfg_apply_neighbor(FALSE, vrf, &nb_addr, as_val);

        bgp_session_t *sess = bgp_vrf_find_session(vrf, &nb_addr);
        if (!sess)
        {
            LOG_WARN("BGP 恢复: session 邻居 %s 创建失败，跳过", ip_val);
            continue;
        }

        bgp_cfg_apply_open_capability(FALSE, sess, open_caps);
    }

    db_result_free(result);
}

/**
 * @brief 从 bgp_neighbor 表恢复地址族邻居到 VRF
 * @param vrf0 目标 VRF（为 NULL 时直接返回）
 */
static void restore_neighbors(dev_ipc_context_t *ctx)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_NEIGHBOR, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *nb_ip = db_row_get_text(row, "neighbor_ip", NULL);
        bgp_afi_t afi = (bgp_afi_t)db_row_get_int(row, "afi", 0);
        bgp_safi_t safi = (bgp_safi_t)db_row_get_int(row, "safi", 0);
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);

        if (!nb_ip)
        {
            continue;
        }

        bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, vrf_id);
        if (!vrf)
        {
            continue;
        }

        net_addr_t nb_addr;
        if (net_addr_from_str(nb_ip, &nb_addr) != 0)
        {
            LOG_WARN("BGP 恢复: peer 邻居地址 %s 解析失败，跳过", nb_ip);
            continue;
        }
        bgp_cfg_apply_af_neighbor(FALSE, vrf, afi, safi, &nb_addr);
    }

    db_result_free(result);
}

/**
 * @brief 从 bgp_instance 表恢复 AF 实例到各 VRF 的 inst_hash
 * @param proto 已恢复的协议对象（不可为 NULL）
 */
static void restore_instances(dev_ipc_context_t *ctx)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);
        bgp_afi_t afi = (bgp_afi_t)db_row_get_int(row, "afi", 0);
        bgp_safi_t safi = (bgp_safi_t)db_row_get_int(row, "safi", 0);

        if (afi == 0 || safi == 0)
        {
            continue;
        }

        bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, vrf_id);
        if (!vrf)
        {
            continue;
        }

        bgp_cfg_apply_instance(FALSE, vrf, afi, safi);
        LOG_INFO("BGP 恢复: VRF %u AF 实例 afi=%u safi=%u", vrf_id, (unsigned)afi, (unsigned)safi);
    }

    db_result_free(result);
}

static void restore_vrf(dev_ipc_context_t *ctx)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_VRF, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);

        bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, vrf_id);
        if (vrf == NULL)
        {
            bgp_vrf_t *new_vrf = bgp_vrf_create(vrf_id);
            g_hash_table_insert(g_bgp_local->protocol->vrf_hash, GINT_TO_POINTER(vrf_id), new_vrf);
            vrf = new_vrf;
        }

        const char *router_id = db_row_get_text(row, "router_id", NULL);
        if (router_id && strcmp(router_id, "0.0.0.0") != 0)
        {
            bgp_cfg_apply_router_id(FALSE, vrf, router_id);
            LOG_INFO("BGP 恢复: VRF %u router-id=%s", vrf_id, router_id);
        }

        uint16_t keepalive = (uint16_t)db_row_get_int(row, "keepalive", BGP_TIMER_DEFAULT_KEEPALIVE);
        uint16_t hold_time = (uint16_t)db_row_get_int(row, "hold_time", BGP_TIMER_DEFAULT_HOLD);
        if (keepalive > 0 && hold_time > keepalive)
        {
            bgp_cfg_apply_timers(FALSE, vrf, keepalive, hold_time);
            LOG_INFO("BGP 恢复: VRF %u keepalive=%u hold=%u", vrf_id, keepalive, hold_time);
        }

        uint16_t connect_retry = (uint16_t)db_row_get_int(row, "connect_retry", BGP_TIMER_DEFAULT_CONNECT_RETRY);
        if (connect_retry > 0)
        {
            bgp_cfg_apply_connect_retry(FALSE, vrf, connect_retry);
            LOG_INFO("BGP 恢复: VRF %u connect-retry=%u", vrf_id, connect_retry);
        }
    }

    db_result_free(result);
}

// ============================================================================
// 启动恢复
// ============================================================================

uint32_t bgp_db_restore(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    uint32_t ret = restore_protocol(ctx);
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    if (!g_bgp_local || !g_bgp_local->protocol)
    {
        return ERRCODE_SUCCESS;
    }

    restore_vrf(ctx);
    restore_sessions(ctx);
    restore_instances(ctx);
    restore_neighbors(ctx);

    return ERRCODE_SUCCESS;
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

    static const db_table_def_t *BGP_TABLES[] = {
        &BGP_PROTOCOL_TABLE, &BGP_VRF_TABLE, &BGP_SESSION_TABLE, &BGP_INSTANCE_TABLE, &BGP_NEIGHBOR_TABLE,
    };

    for (size_t i = 0; i < G_N_ELEMENTS(BGP_TABLES); i++)
    {
        int ret = db_rpc_create_table_from_def(ctx, BGP_TABLES[i]);
        if (ret != ERRCODE_SUCCESS)
        {
            LOG_ERROR("BGP 建表失败: %s", BGP_TABLES[i]->table_name);
            return -1;
        }
        LOG_INFO("BGP 数据库表 %s 已就绪", BGP_TABLES[i]->table_name);
    }

    return 0;
}

// ============================================================================
// 默认值写入
// ============================================================================

/**
 * @brief 检查指定表是否为空（查询失败按空表处理，保守写入默认值）
 */
static gboolean table_is_empty(dev_ipc_context_t *ctx, const char *table_name)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, table_name, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return TRUE;
    }
    gboolean empty = (result->num_rows == 0);
    db_result_free(result);
    return empty;
}

/**
 * @brief 按表逐一检查并写入各自的默认值
 *
 * 每张表独立判断：为空则写入，非空则跳过，互不影响。
 */
static void write_defaults(dev_ipc_context_t *ctx)
{
    if (table_is_empty(ctx, BGP_TABLE_PROTOCOL))
    {
        LOG_INFO("BGP %s 表为空，写入默认配置", BGP_TABLE_PROTOCOL);
        /* bgp_db_set_as(ctx, DEFAULT_AS); */
    }

    if (table_is_empty(ctx, BGP_TABLE_SESSION))
    {
        LOG_INFO("BGP %s 表为空，写入默认配置", BGP_TABLE_SESSION);
        /* bgp_db_set_session(ctx, ...); */
    }

    if (table_is_empty(ctx, BGP_TABLE_NEIGHBOR))
    {
        LOG_INFO("BGP %s 表为空，写入默认配置", BGP_TABLE_NEIGHBOR);
        /* bgp_db_set_neighbor(ctx, ...); */
    }

    if (table_is_empty(ctx, BGP_TABLE_VRF))
    {
        LOG_INFO("BGP %s 表为空，写入默认 VRF 定时器", BGP_TABLE_VRF);
        bgp_db_set_vrf_timers(ctx, BGP_VRF_PUBLIC_ID, BGP_TIMER_DEFAULT_KEEPALIVE, BGP_TIMER_DEFAULT_HOLD);
        bgp_db_set_vrf_connect_retry(ctx, BGP_VRF_PUBLIC_ID, BGP_TIMER_DEFAULT_CONNECT_RETRY);
    }
}

void bgp_db_ensure_defaults(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return;
    }
    write_defaults(ctx);
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

int bgp_db_set_session(dev_ipc_context_t *ctx, uint32_t vrf_id, const char *neighbor_ip, uint32_t remote_as)
{
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    db_condition_t key_conditions[] = {
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "neighbor_ip", neighbor_ip);
    db_record_set_int(rec, "remote_as", (int64_t)remote_as);
    db_record_set_int(rec, "vrf_id", (int64_t)vrf_id);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_SESSION, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 session vrf_id=%u neighbor=%s 失败", vrf_id, neighbor_ip);
        return -1;
    }

    LOG_INFO("BGP session vrf_id=%u neighbor=%s AS=%u 已写入", vrf_id, neighbor_ip, remote_as);
    return 0;
}

int bgp_db_del_session(dev_ipc_context_t *ctx, uint32_t vrf_id, const char *neighbor_ip)
{
    if (!ctx)
    {
        return -1;
    }

    int rows;

    if (!neighbor_ip)
    {
        /* 删除 vrf 内所有 session */
        db_condition_t vrf_cond = {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)};
        db_filter_t vrf_filter = {.conditions = &vrf_cond, .num_conditions = 1};
        rows = db_rpc_delete(ctx, BGP_TABLE_SESSION, &vrf_filter);
        db_value_free(&vrf_cond.value);
    }
    else
    {
        db_condition_t conditions[] = {
            {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
            {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
        };
        db_filter_t filter = {.conditions = conditions, .num_conditions = G_N_ELEMENTS(conditions)};

        /* 同时清理 neighbor 记录 */
        db_condition_t nb_cond = {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)};
        db_filter_t nb_filter = {.conditions = &nb_cond, .num_conditions = 1};
        db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &nb_filter);
        db_value_free(&nb_cond.value);

        rows = db_rpc_delete(ctx, BGP_TABLE_SESSION, &filter);
        db_value_free(&conditions[0].value);
        db_value_free(&conditions[1].value);
    }

    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 session 失败");
        return -1;
    }

    LOG_INFO("BGP 删除 session（vrf_id=%u neighbor=%s），影响行数: %d", vrf_id, neighbor_ip ? neighbor_ip : "*", rows);
    return rows;
}

// ============================================================================
// BGP Neighbor 操作（地址族）
// ============================================================================

int bgp_db_set_neighbor(dev_ipc_context_t *ctx, uint32_t vrf_id, const char *neighbor_ip, bgp_afi_t afi,
                        bgp_safi_t safi)
{
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    db_condition_t key_conditions[] = {
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)afi)},
        {.field_name = "safi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)safi)},
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    /* neighbor 记录以 (vrf_id, afi, safi, neighbor_ip) 四列联合为键，已存在时无需更新，直接幂等插入 */
    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, BGP_TABLE_NEIGHBOR, &key_filter, &exists);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);
    db_value_free(&key_conditions[2].value);
    db_value_free(&key_conditions[3].value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 查询 neighbor 存在性失败");
        return -1;
    }

    if (exists)
    {
        LOG_INFO("BGP neighbor vrf=%u %s afi=%u safi=%u 已存在", vrf_id, neighbor_ip, (unsigned)afi, (unsigned)safi);
        return 0;
    }

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "vrf_id", (int64_t)vrf_id);
    db_record_set_int(rec, "afi", (int64_t)afi);
    db_record_set_int(rec, "safi", (int64_t)safi);
    db_record_set_text(rec, "neighbor_ip", neighbor_ip);
    ret = db_rpc_insert_record(ctx, BGP_TABLE_NEIGHBOR, rec);
    db_record_free(rec);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 插入 neighbor vrf=%u %s afi=%u safi=%u 失败", vrf_id, neighbor_ip, (unsigned)afi,
                  (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP neighbor vrf=%u %s afi=%u safi=%u 已使能", vrf_id, neighbor_ip, (unsigned)afi, (unsigned)safi);
    return 0;
}

int bgp_db_del_neighbor(dev_ipc_context_t *ctx, uint32_t vrf_id, const char *neighbor_ip, bgp_afi_t afi,
                        bgp_safi_t safi)
{
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    db_condition_t conditions[] = {
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)afi)},
        {.field_name = "safi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)safi)},
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
    };
    db_filter_t filter = {.conditions = conditions, .num_conditions = G_N_ELEMENTS(conditions)};

    int rows = db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &filter);
    db_value_free(&conditions[0].value);
    db_value_free(&conditions[1].value);
    db_value_free(&conditions[2].value);
    db_value_free(&conditions[3].value);

    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 neighbor 失败");
        return -1;
    }

    LOG_INFO("BGP 删除 neighbor vrf=%u %s afi=%u safi=%u，影响行数: %d", vrf_id, neighbor_ip, (unsigned)afi,
             (unsigned)safi, rows);
    return rows;
}

// ============================================================================
// BGP VRF 操作（router-id）
// ============================================================================

int bgp_db_set_vrf_router_id(dev_ipc_context_t *ctx, uint32_t vrf_id, const char *router_id)
{
    if (!ctx || !router_id)
    {
        return -1;
    }

    db_condition_t key_cond = {
        .field_name = "vrf_id",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)vrf_id),
    };
    db_filter_t key_filter = {.conditions = &key_cond, .num_conditions = 1};

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "vrf_id", (int64_t)vrf_id);
    db_record_set_text(rec, "router_id", router_id);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_VRF, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_cond.value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 VRF %u router-id=%s 失败", vrf_id, router_id);
        return -1;
    }

    LOG_INFO("BGP VRF %u router-id=%s 已写入", vrf_id, router_id);
    return 0;
}

int bgp_db_del_vrf_router_id(dev_ipc_context_t *ctx, uint32_t vrf_id)
{
    if (!ctx)
    {
        return -1;
    }

    db_condition_t cond = {
        .field_name = "vrf_id",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)vrf_id),
    };
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};

    int rows = db_rpc_delete(ctx, BGP_TABLE_VRF, &filter);
    db_value_free(&cond.value);

    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 VRF %u router-id 失败", vrf_id);
        return -1;
    }

    LOG_INFO("BGP VRF %u router-id 已删除，影响行数: %d", vrf_id, rows);
    return rows;
}

// ============================================================================
// BGP VRF 操作（定时器）
// ============================================================================

int bgp_db_set_vrf_timers(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t keepalive, uint16_t hold_time)
{
    if (!ctx)
    {
        return -1;
    }

    db_condition_t key_cond = {
        .field_name = "vrf_id",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)vrf_id),
    };
    db_filter_t key_filter = {.conditions = &key_cond, .num_conditions = 1};

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "vrf_id", (int64_t)vrf_id);
    db_record_set_int(rec, "keepalive", (int64_t)keepalive);
    db_record_set_int(rec, "hold_time", (int64_t)hold_time);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_VRF, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_cond.value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 VRF %u timers keepalive=%u hold=%u 失败", vrf_id, keepalive, hold_time);
        return -1;
    }

    LOG_INFO("BGP VRF %u timers keepalive=%u hold=%u 已写入", vrf_id, keepalive, hold_time);
    return 0;
}

int bgp_db_del_vrf_timers(dev_ipc_context_t *ctx, uint32_t vrf_id)
{
    /* 重置为默认值 */
    return bgp_db_set_vrf_timers(ctx, vrf_id, BGP_TIMER_DEFAULT_KEEPALIVE, BGP_TIMER_DEFAULT_HOLD);
}

// ============================================================================
// BGP VRF 操作（connect-retry 定时器）
// ============================================================================

int bgp_db_set_vrf_connect_retry(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t connect_retry)
{
    if (!ctx)
    {
        return -1;
    }

    db_condition_t key_cond = {
        .field_name = "vrf_id",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)vrf_id),
    };
    db_filter_t key_filter = {.conditions = &key_cond, .num_conditions = 1};

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "vrf_id", (int64_t)vrf_id);
    db_record_set_int(rec, "connect_retry", (int64_t)connect_retry);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_VRF, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_cond.value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 VRF %u connect-retry=%u 失败", vrf_id, connect_retry);
        return -1;
    }

    LOG_INFO("BGP VRF %u connect-retry=%u 已写入", vrf_id, connect_retry);
    return 0;
}

int bgp_db_del_vrf_connect_retry(dev_ipc_context_t *ctx, uint32_t vrf_id)
{
    /* 重置为默认值 */
    return bgp_db_set_vrf_connect_retry(ctx, vrf_id, BGP_TIMER_DEFAULT_CONNECT_RETRY);
}

// ============================================================================
// BGP 地址族实例操作（bgp_instance 表）
// ============================================================================

int bgp_db_set_instance(dev_ipc_context_t *ctx, uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!ctx)
    {
        return -1;
    }

    db_condition_t key_conditions[] = {
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)afi)},
        {.field_name = "safi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)safi)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, BGP_TABLE_INSTANCE, &key_filter, &exists);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);
    db_value_free(&key_conditions[2].value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 查询 instance 存在性失败");
        return -1;
    }
    if (exists)
    {
        LOG_INFO("BGP instance vrf=%u afi=%u safi=%u 已存在", vrf_id, (unsigned)afi, (unsigned)safi);
        return 0;
    }

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "vrf_id", (int64_t)vrf_id);
    db_record_set_int(rec, "afi", (int64_t)afi);
    db_record_set_int(rec, "safi", (int64_t)safi);
    ret = db_rpc_insert_record(ctx, BGP_TABLE_INSTANCE, rec);
    db_record_free(rec);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 插入 instance vrf=%u afi=%u safi=%u 失败", vrf_id, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP instance vrf=%u afi=%u safi=%u 已写入", vrf_id, (unsigned)afi, (unsigned)safi);
    return 0;
}

int bgp_db_del_instance(dev_ipc_context_t *ctx, uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!ctx)
    {
        return -1;
    }

    db_condition_t conditions[] = {
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)afi)},
        {.field_name = "safi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)safi)},
    };
    db_filter_t filter = {.conditions = conditions, .num_conditions = G_N_ELEMENTS(conditions)};

    int rows = db_rpc_delete(ctx, BGP_TABLE_INSTANCE, &filter);
    db_value_free(&conditions[0].value);
    db_value_free(&conditions[1].value);
    db_value_free(&conditions[2].value);

    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 instance 失败");
        return -1;
    }

    LOG_INFO("BGP 删除 instance vrf=%u afi=%u safi=%u，影响行数: %d", vrf_id, (unsigned)afi, (unsigned)safi, rows);
    return rows;
}

int bgp_db_set_session_caps(dev_ipc_context_t *ctx, uint32_t vrf_id, const char *neighbor_ip, uint32_t open_caps)
{
    if (!ctx || !neighbor_ip)
    {
        return -1;
    }

    db_condition_t key_conditions[] = {
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "neighbor_ip", neighbor_ip);
    db_record_set_int(rec, "vrf_id", (int64_t)vrf_id);
    db_record_set_int(rec, "open_caps", (int64_t)open_caps);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_SESSION, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 session open_caps vrf_id=%u neighbor=%s 失败", vrf_id, neighbor_ip);
        return -1;
    }

    LOG_INFO("BGP session neighbor=%s open_caps=0x%02X 已写入", neighbor_ip, open_caps);
    return 0;
}

int bgp_db_del_neighbors_by_afi(dev_ipc_context_t *ctx, uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!ctx)
    {
        return -1;
    }

    db_condition_t conditions[] = {
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)afi)},
        {.field_name = "safi", .op = DB_CMP_EQ, .value = db_value_int((int64_t)safi)},
    };
    db_filter_t filter = {.conditions = conditions, .num_conditions = G_N_ELEMENTS(conditions)};

    int rows = db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &filter);
    db_value_free(&conditions[0].value);
    db_value_free(&conditions[1].value);
    db_value_free(&conditions[2].value);

    if (rows < 0)
    {
        LOG_ERROR("BGP 批量删除 neighbor vrf=%u afi=%u safi=%u 失败", vrf_id, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP 批量删除 neighbor vrf=%u afi=%u safi=%u，影响行数: %d", vrf_id, (unsigned)afi, (unsigned)safi, rows);
    return rows;
}
