/**
 * @file   bgp_bdr.c
 * @brief  BGP 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/03/04
 */
#include "bgp_bdr.h"

#include <string.h>

#include "bgp_cli.h"
#include "bgp_db.h"
#include "bgp_main.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// AFI/SAFI 转换辅助
// ============================================================================

/**
 * @brief 将 bgp_instance 表中的整数 afi/safi 转为 bgp_neighbor 表使用的文本标识
 * @return 文本字符串（静态常量），不支持的组合返回 NULL
 */
static const char *afi_safi_to_str(int64_t afi, int64_t safi)
{
    if (afi == 1 && safi == 1)
    {
        return "ipv4-unicast"; /* BGP_AFI_IPV4 + BGP_SAFI_UNICAST */
    }
    if (afi == 2 && safi == 1)
    {
        return "ipv6-unicast"; /* BGP_AFI_IPV6 + BGP_SAFI_UNICAST */
    }
    return NULL;
}

// ============================================================================
// 内部辅助
// ============================================================================

// ============================================================================
// 各表配置追加函数
// ============================================================================

/**
 * @brief 追加协议头配置行（bgp <as> / bgp router-id）
 * @return TRUE 表示协议已配置，FALSE 表示无配置
 */
static gboolean bdr_append_protocol(dev_ipc_context_t *ctx, GString *out)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_PROTOCOL, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result ||
        result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return FALSE;
    }

    db_row_t *row = result->rows[0];
    uint32_t as_number = (uint32_t)db_row_get_int(row, "as_number", 0);

    g_string_append(out, "!\r\n");
    g_string_append_printf(out, "bgp %u\r\n", as_number);

    db_result_free(result);
    return TRUE;
}

/**
 * @brief 追加 VRF 级配置（router-id）
 */
static void bdr_append_vrf_config(dev_ipc_context_t *ctx, GString *out)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_VRF, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *router_id = db_row_get_text(row, "router_id", NULL);
        int64_t keepalive = db_row_get_int(row, "keepalive", BGP_TIMER_DEFAULT_KEEPALIVE);
        int64_t hold_time = db_row_get_int(row, "hold_time", BGP_TIMER_DEFAULT_HOLD);
        int64_t connect_retry = db_row_get_int(row, "connect_retry", BGP_TIMER_DEFAULT_CONNECT_RETRY);

        if (router_id && strcmp(router_id, "0.0.0.0") != 0)
        {
            g_string_append_printf(out, " router-id %s\r\n", router_id);
        }

        if (keepalive != BGP_TIMER_DEFAULT_KEEPALIVE || hold_time != BGP_TIMER_DEFAULT_HOLD)
        {
            g_string_append_printf(out, " timer keepalive %ld hold %ld\r\n", keepalive, hold_time);
        }

        if (connect_retry != BGP_TIMER_DEFAULT_CONNECT_RETRY)
        {
            g_string_append_printf(out, " timer connect-retry %ld\r\n", connect_retry);
        }
    }

    db_result_free(result);
}

/**
 * @brief 追加会话配置行（neighbor <ip> as <as>）
 */
static void bdr_append_sessions(dev_ipc_context_t *ctx, GString *out)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_SESSION, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *ip = db_row_get_text(row, "neighbor_ip", NULL);
        int64_t remote_as = db_row_get_int(row, "remote_as", 0);

        if (ip)
        {
            g_string_append_printf(out, " neighbor %s as %ld\r\n", ip, remote_as);
        }
    }

    db_result_free(result);
}

/**
 * @brief 追加某 AF 下使能邻居配置行（neighbor <ip> enable）
 * @param afi  整数 AFI（与 bgp_neighbor 表存储一致）
 * @param safi 整数 SAFI
 */
static void bdr_append_af_peers(dev_ipc_context_t *ctx, GString *out, int64_t afi, int64_t safi)
{
    db_condition_t conds[] = {
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int(afi)},
        {.field_name = "safi", .op = DB_CMP_EQ, .value = db_value_int(safi)},
    };
    db_filter_t filter = {.conditions = conds, .num_conditions = G_N_ELEMENTS(conds)};

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_NEIGHBOR, NULL, 0, &filter, &result) != ERRCODE_SUCCESS || !result)
    {
        db_value_free(&conds[0].value);
        db_value_free(&conds[1].value);
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *ip = db_row_get_text(row, "neighbor_ip", NULL);
        if (ip)
        {
            g_string_append_printf(out, "  neighbor %s enable\r\n", ip);
        }
    }

    db_result_free(result);
    db_value_free(&conds[0].value);
    db_value_free(&conds[1].value);
}

/**
 * @brief 追加单个 AF 完整配置块（af <afi> / 各子表配置 / !）
 * @param afi     整数 AFI
 * @param safi    整数 SAFI
 */
static void bdr_append_af_block(dev_ipc_context_t *ctx, GString *out, const char *afi_str, int64_t afi, int64_t safi)
{
    g_string_append_printf(out, " af %s\r\n", afi_str);

    /* AF 下各子表 BDR，按需扩展 */
    bdr_append_af_peers(ctx, out, afi, safi);

    g_string_append(out, " !\r\n");
}

/**
 * @brief 遍历 bgp_instance 表，对每个 AF 实例输出完整配置块
 */
static void bdr_append_af_instances(dev_ipc_context_t *ctx, GString *out)
{
    db_result_t *inst_result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_INSTANCE, NULL, 0, NULL, &inst_result) != ERRCODE_SUCCESS || !inst_result ||
        inst_result->num_rows == 0)
    {
        if (inst_result)
        {
            db_result_free(inst_result);
        }
        return;
    }

    for (uint32_t i = 0; i < inst_result->num_rows; i++)
    {
        db_row_t *row = inst_result->rows[i];
        int64_t afi_int = db_row_get_int(row, "afi", 0);
        int64_t safi_int = db_row_get_int(row, "safi", 0);

        const char *afi_str = afi_safi_to_str(afi_int, safi_int);
        if (!afi_str)
        {
            continue;
        }

        bdr_append_af_block(ctx, out, afi_str, afi_int, safi_int);
    }

    db_result_free(inst_result);
}

// ============================================================================
// 公共 API
// ============================================================================

void bgp_bdr_show_config(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = g_bgp_local->dev_ipc_ctx;
    GString *out = g_string_new("");
    if (!out)
    {
        (void)bgp_cli_send_chunked_response(msg, NULL);
        return;
    }

    if (!bdr_append_protocol(ctx, out))
    {
        (void)bgp_cli_send_chunked_response(msg, out);
        return;
    }

    bdr_append_vrf_config(ctx, out);
    bdr_append_sessions(ctx, out);
    bdr_append_af_instances(ctx, out);
    g_string_append(out, "!\r\n");

    (void)bgp_cli_send_chunked_response(msg, out);
}
