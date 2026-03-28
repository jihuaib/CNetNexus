/**
 * @file   bgp_db.c
 * @brief  BGP 模块数据库操作实现（封装 db_rpc 调用）
 * @author jhb
 * @date   2026/02/23
 */
#include "bgp_db.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bgp_cli.h"
#include "bgp_main.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

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
    {"source_interface", DB_TYPE_TEXT, DB_COL_NONE, NULL},
    {"ebgp_multihop", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
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
    {"import_protos", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"}, /* 已导入协议位掩码 */
};

/* bgp_instance 表定义 */
static const db_table_def_t BGP_INSTANCE_TABLE = {
    .table_name = BGP_TABLE_INSTANCE,
    .cols = BGP_INSTANCE_COLS,
    .num_cols = G_N_ELEMENTS(BGP_INSTANCE_COLS),
};

/* bgp_vrf 表列定义 */
static const db_column_def_t BGP_VRF_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},     {"router_id", DB_TYPE_TEXT, DB_COL_NOT_NULL, "0.0.0.0"},
    {"keepalive", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "60"},     {"hold_time", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "180"},
    {"connect_retry", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "10"},
};

/* bgp_vrf 表定义 */
static const db_table_def_t BGP_VRF_TABLE = {
    .table_name = BGP_TABLE_VRF,
    .cols = BGP_VRF_COLS,
    .num_cols = G_N_ELEMENTS(BGP_VRF_COLS),
};

/* 前向声明（write_defaults 中调用） */
int bgp_db_set_vrf_timers(uint32_t vrf_id, uint16_t keepalive, uint16_t hold_time);
int bgp_db_set_vrf_connect_retry(uint32_t vrf_id, uint16_t connect_retry);

// ============================================================================
// 启动恢复 - 内部函数（按表拆分）
// ============================================================================

/**
 * @brief 从 bgp_protocol 表恢复协议对象
 * @return 恢复的 bgp_protocol_t 指针，无配置或失败返回 NULL
 */
static uint32_t restore_protocol(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_PROTOCOL, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    /*
     * DB RPC layer may return NULL result for empty SELECT payload.
     * Treat it as "no config" instead of restore failure.
     */
    if (!result || result->num_rows == 0)
    {
        db_result_free(result);
        LOG_INFO("BGP database has no config, skipping restore");
        return ERRCODE_SUCCESS;
    }

    db_row_t *row = result->rows[0];
    uint32_t as_number = (uint32_t)db_row_get_int(row, "as_number", 0);

    if (as_number != 0)
    {
        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_PROTOCOL;
        apply.isNo = false;
        apply.u.protocol.as_number = as_number;
        if (bgp_worker_dispatch_apply(&apply) == 0 && apply.rc == BGP_APPLY_RC_OK)
        {
            LOG_INFO("BGP protocol restored: AS %u", as_number);
        }
        else
        {
            LOG_ERROR("BGP restore: Protocol creation failed (as=%u)", as_number);
        }
    }

    db_result_free(result);
    return ERRCODE_SUCCESS;
}

static void restore_sessions(void)
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

/**
 * @brief 从 bgp_neighbor 表恢复地址族邻居到 VRF
 * @param vrf0 目标 VRF（为 NULL 时直接返回）
 */
static void restore_neighbors(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
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
        bgp_afi_t afi = (bgp_afi_t)db_row_get_int(row, "afi", 0);
        bgp_safi_t safi = (bgp_safi_t)db_row_get_int(row, "safi", 0);
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);

        if (!nb_ip)
        {
            continue;
        }

        net_addr_t nb_addr;
        if (net_addr_from_str(nb_ip, &nb_addr) != 0)
        {
            LOG_WARN("BGP restore: Peer neighbor address %s parse failed, skipping", nb_ip);
            continue;
        }

        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_AF_NEIGHBOR;
        apply.isNo = false;
        apply.vrf_id = vrf_id;
        apply.u.af_neighbor.afi = afi;
        apply.u.af_neighbor.safi = safi;
        apply.u.af_neighbor.addr = nb_addr;
        (void)bgp_worker_dispatch_apply(&apply);
    }

    db_result_free(result);
}

/**
 * @brief 从 bgp_instance 表恢复 AF 实例到各 VRF 的 inst_hash
 * @param proto 已恢复的协议对象（不可为 NULL）
 */
static void restore_instances(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

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

        /* 恢复 AF 实例 */
        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_ADDR_FAMILY;
        apply.isNo = false;
        apply.vrf_id = vrf_id;
        apply.u.instance.afi = afi;
        apply.u.instance.safi = safi;
        if (bgp_worker_dispatch_apply(&apply) != 0 || apply.rc != BGP_APPLY_RC_OK)
        {
            LOG_WARN("BGP restore: AF instance vrf=%u afi=%u safi=%u failed", vrf_id, (unsigned)afi, (unsigned)safi);
            continue;
        }
        LOG_INFO("BGP restore: VRF %u AF instance afi=%u safi=%u", vrf_id, (unsigned)afi, (unsigned)safi);

        /* 恢复 import-route（当前仅支持 static） */
        uint32_t import_protos = (uint32_t)db_row_get_int(row, "import_protos", 0);
        if (import_protos & (1u << ROUTE_PROTOCOL_STATIC))
        {
            bgp_apply_cmd_t imp;
            memset(&imp, 0, sizeof(imp));
            imp.group_id = BGP_CLI_GROUP_ID_IMPORT_ROUTE;
            imp.isNo = false;
            imp.vrf_id = vrf_id;
            imp.u.import_route.afi = afi;
            imp.u.import_route.safi = safi;
            imp.u.import_route.import_proto = ROUTE_PROTOCOL_STATIC;
            (void)bgp_worker_dispatch_apply(&imp);

            /* 重新订阅路由模块（fire-and-forget） */
            route_subscribe_req_t *req = g_malloc(sizeof(route_subscribe_req_t));
            req->protocol = ROUTE_PROTOCOL_STATIC;
            req->vrf_id = ROUTE_VRF_DEFAULT;
            req->flags = ROUTE_SUBSCRIBE_FLAG_FULL;
            dev_ipc_message_t *sub_msg =
                dev_ipc_message_create(ROUTE_MSG_TYPE_SUBSCRIBE, DEV_MODULE_ID_BGP, DEV_MODULE_ID_ROUTE, 0, req,
                                       sizeof(route_subscribe_req_t), g_free);
            if (sub_msg)
            {
                dev_ipc_send(ctx, DEV_MODULE_ID_ROUTE, sub_msg);
                dev_ipc_message_free(sub_msg);
            }
            LOG_INFO("BGP restore: VRF %u afi=%u safi=%u import_protos=0x%08X，已重新订阅路由模块", vrf_id,
                     (unsigned)afi, (unsigned)safi, import_protos);
        }
    }

    db_result_free(result);
}

static void restore_vrf(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_VRF, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);

        const char *router_id = db_row_get_text(row, "router_id", NULL);
        if (router_id && strcmp(router_id, "0.0.0.0") != 0)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_ROUTER_ID;
            apply.isNo = false;
            apply.vrf_id = vrf_id;
            snprintf(apply.u.router_id.id, sizeof(apply.u.router_id.id), "%s", router_id);
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %u router-id=%s", vrf_id, router_id);
        }

        uint16_t keepalive = (uint16_t)db_row_get_int(row, "keepalive", BGP_TIMER_DEFAULT_KEEPALIVE);
        uint16_t hold_time = (uint16_t)db_row_get_int(row, "hold_time", BGP_TIMER_DEFAULT_HOLD);
        if (keepalive > 0 && hold_time > keepalive)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_TIMERS;
            apply.isNo = false;
            apply.vrf_id = vrf_id;
            apply.u.timers.keepalive = keepalive;
            apply.u.timers.hold_time = hold_time;
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %u keepalive=%u hold=%u", vrf_id, keepalive, hold_time);
        }

        uint16_t connect_retry = (uint16_t)db_row_get_int(row, "connect_retry", BGP_TIMER_DEFAULT_CONNECT_RETRY);
        if (connect_retry > 0)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_CONNECT_RETRY;
            apply.isNo = false;
            apply.vrf_id = vrf_id;
            apply.u.connect_retry.interval = connect_retry;
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %u connect-retry=%u", vrf_id, connect_retry);
        }
    }

    db_result_free(result);
}

// ============================================================================
// 启动恢复
// ============================================================================

uint32_t bgp_db_restore(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    uint32_t ret = restore_protocol();
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    if (!g_bgp_local || !g_bgp_work_local->protocol)
    {
        return ERRCODE_SUCCESS;
    }

    restore_vrf();
    restore_sessions();
    restore_instances();
    restore_neighbors();

    return ERRCODE_SUCCESS;
}

// ============================================================================
// 建表初始化
// ============================================================================

int bgp_db_init(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
            LOG_ERROR("BGP table creation failed: %s", BGP_TABLES[i]->table_name);
            return -1;
        }
        LOG_INFO("BGP database table %s ready", BGP_TABLES[i]->table_name);
    }

    return 0;
}

// ============================================================================
// 默认值写入
// ============================================================================

/**
 * @brief 检查指定表是否为空（查询失败按空表处理，保守写入默认值）
 */
static gboolean table_is_empty(const char *table_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return TRUE;
    }

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
static void write_defaults(void)
{
    if (table_is_empty(BGP_TABLE_PROTOCOL))
    {
        LOG_INFO("BGP %s table empty, writing default config", BGP_TABLE_PROTOCOL);
        /* bgp_db_set_as(DEFAULT_AS); */
    }

    if (table_is_empty(BGP_TABLE_SESSION))
    {
        LOG_INFO("BGP %s table empty, writing default config", BGP_TABLE_SESSION);
        /* bgp_db_set_session(...); */
    }

    if (table_is_empty(BGP_TABLE_NEIGHBOR))
    {
        LOG_INFO("BGP %s table empty, writing default config", BGP_TABLE_NEIGHBOR);
        /* bgp_db_set_neighbor(...); */
    }

    if (table_is_empty(BGP_TABLE_VRF))
    {
        LOG_INFO("BGP %s table empty, writing default VRF timers", BGP_TABLE_VRF);
        bgp_db_set_vrf_timers(BGP_VRF_PUBLIC_ID, BGP_TIMER_DEFAULT_KEEPALIVE, BGP_TIMER_DEFAULT_HOLD);
        bgp_db_set_vrf_connect_retry(BGP_VRF_PUBLIC_ID, BGP_TIMER_DEFAULT_CONNECT_RETRY);
    }
}

void bgp_db_ensure_defaults(void)
{
    if (!bgp_local_ipc_ctx())
    {
        return;
    }
    write_defaults();
}

// ============================================================================
// 写入（插入或更新）AS 号
// ============================================================================

int bgp_db_set_as(uint32_t as_number)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to write AS number %u", as_number);
        return -1;
    }

    LOG_INFO("BGP AS number %u written", as_number);
    return 0;
}

// ============================================================================
// 删除 AS 配置
// ============================================================================

int bgp_db_del_as(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    int rows = db_rpc_delete(ctx, BGP_TABLE_PROTOCOL, NULL);
    if (rows < 0)
    {
        LOG_ERROR("BGP failed to delete AS config");
        return -1;
    }

    LOG_INFO("BGP deleted AS config, affected rows: %d", rows);
    return rows;
}

// ============================================================================
// BGP Session 操作
// ============================================================================

int bgp_db_set_session(uint32_t vrf_id, const char *neighbor_ip, uint32_t remote_as)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to delete session");
        return -1;
    }

    LOG_INFO("BGP deleted session (vrf_id=%u neighbor=%s), affected rows: %d", vrf_id, neighbor_ip ? neighbor_ip : "*",
             rows);
    return rows;
}

// ============================================================================
// BGP Neighbor 操作（地址族）
// ============================================================================

int bgp_db_set_neighbor(uint32_t vrf_id, const char *neighbor_ip, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to query neighbor existence");
        return -1;
    }

    if (exists)
    {
        LOG_INFO("BGP neighbor vrf=%u %s afi=%u safi=%u already exists", vrf_id, neighbor_ip, (unsigned)afi,
                 (unsigned)safi);
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
        LOG_ERROR("BGP failed to insert neighbor vrf=%u %s afi=%u safi=%u", vrf_id, neighbor_ip, (unsigned)afi,
                  (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP neighbor vrf=%u %s afi=%u safi=%u enabled", vrf_id, neighbor_ip, (unsigned)afi, (unsigned)safi);
    return 0;
}

int bgp_db_del_neighbor(uint32_t vrf_id, const char *neighbor_ip, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to delete neighbor");
        return -1;
    }

    LOG_INFO("BGP deleted neighbor vrf=%u %s afi=%u safi=%u, affected rows: %d", vrf_id, neighbor_ip, (unsigned)afi,
             (unsigned)safi, rows);
    return rows;
}

// ============================================================================
// BGP VRF 操作（router-id）
// ============================================================================

int bgp_db_set_vrf_router_id(uint32_t vrf_id, const char *router_id)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to write VRF %u router-id=%s", vrf_id, router_id);
        return -1;
    }

    LOG_INFO("BGP VRF %u router-id=%s written", vrf_id, router_id);
    return 0;
}

int bgp_db_del_vrf_router_id(uint32_t vrf_id)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to delete VRF %u router-id", vrf_id);
        return -1;
    }

    LOG_INFO("BGP VRF %u router-id deleted, affected rows: %d", vrf_id, rows);
    return rows;
}

// ============================================================================
// BGP VRF 操作（定时器）
// ============================================================================

int bgp_db_set_vrf_timers(uint32_t vrf_id, uint16_t keepalive, uint16_t hold_time)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to write VRF %u timers keepalive=%u hold=%u", vrf_id, keepalive, hold_time);
        return -1;
    }

    LOG_INFO("BGP VRF %u timers keepalive=%u hold=%u written", vrf_id, keepalive, hold_time);
    return 0;
}

int bgp_db_del_vrf_timers(uint32_t vrf_id)
{
    /* 重置为默认值 */
    return bgp_db_set_vrf_timers(vrf_id, BGP_TIMER_DEFAULT_KEEPALIVE, BGP_TIMER_DEFAULT_HOLD);
}

// ============================================================================
// BGP VRF 操作（connect-retry 定时器）
// ============================================================================

int bgp_db_set_vrf_connect_retry(uint32_t vrf_id, uint16_t connect_retry)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to write VRF %u connect-retry=%u", vrf_id, connect_retry);
        return -1;
    }

    LOG_INFO("BGP VRF %u connect-retry=%u written", vrf_id, connect_retry);
    return 0;
}

int bgp_db_del_vrf_connect_retry(uint32_t vrf_id)
{
    /* 重置为默认值 */
    return bgp_db_set_vrf_connect_retry(vrf_id, BGP_TIMER_DEFAULT_CONNECT_RETRY);
}

// ============================================================================
// BGP 地址族实例操作（bgp_instance 表）
// ============================================================================

int bgp_db_set_instance(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to query instance existence");
        return -1;
    }
    if (exists)
    {
        LOG_INFO("BGP instance vrf=%u afi=%u safi=%u already exists", vrf_id, (unsigned)afi, (unsigned)safi);
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
        LOG_ERROR("BGP failed to insert instance vrf=%u afi=%u safi=%u", vrf_id, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP instance vrf=%u afi=%u safi=%u written", vrf_id, (unsigned)afi, (unsigned)safi);
    return 0;
}

int bgp_db_del_instance(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to delete instance");
        return -1;
    }

    LOG_INFO("BGP deleted instance vrf=%u afi=%u safi=%u, affected rows: %d", vrf_id, (unsigned)afi, (unsigned)safi,
             rows);
    return rows;
}

int bgp_db_set_import_protos(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi, uint32_t import_protos)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "vrf_id", (int64_t)vrf_id);
    db_record_set_int(rec, "afi", (int64_t)afi);
    db_record_set_int(rec, "safi", (int64_t)safi);
    db_record_set_int(rec, "import_protos", (int64_t)import_protos);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_INSTANCE, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);
    db_value_free(&key_conditions[2].value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 instance import_protos vrf=%u afi=%u safi=%u 失败", vrf_id, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP instance vrf=%u afi=%u safi=%u import_protos=0x%08X 已写入", vrf_id, (unsigned)afi, (unsigned)safi,
             import_protos);
    return 0;
}

int bgp_db_set_session_caps(uint32_t vrf_id, const char *neighbor_ip, uint32_t open_caps)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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

    db_condition_t key_conditions[] = {
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "source_interface", if_name);

    int rows = db_rpc_update_record(ctx, BGP_TABLE_SESSION, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);

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

    db_condition_t key_conditions[] = {
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "source_interface", "");

    int rows = db_rpc_update_record(ctx, BGP_TABLE_SESSION, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);

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

    db_condition_t key_conditions[] = {
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "ebgp_multihop", (int64_t)ttl);

    int rows = db_rpc_update_record(ctx, BGP_TABLE_SESSION, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);

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

    db_condition_t key_conditions[] = {
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int((int64_t)vrf_id)},
    };
    db_filter_t key_filter = {.conditions = key_conditions, .num_conditions = G_N_ELEMENTS(key_conditions)};

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "ebgp_multihop", 0);

    int rows = db_rpc_update_record(ctx, BGP_TABLE_SESSION, rec, &key_filter);
    db_record_free(rec);
    db_value_free(&key_conditions[0].value);
    db_value_free(&key_conditions[1].value);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to clear session ebgp-multihop vrf_id=%u neighbor=%s", vrf_id, neighbor_ip);
        return -1;
    }

    LOG_INFO("BGP session ebgp-multihop vrf_id=%u neighbor=%s cleared", vrf_id, neighbor_ip);
    return 0;
}

int bgp_db_del_neighbors_by_afi(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        LOG_ERROR("BGP failed to batch delete neighbor vrf=%u afi=%u safi=%u", vrf_id, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP batch deleted neighbor vrf=%u afi=%u safi=%u, affected rows: %d", vrf_id, (unsigned)afi,
             (unsigned)safi, rows);
    return rows;
}
