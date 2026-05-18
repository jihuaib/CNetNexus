/**
 * @file   bgp_bdr.c
 * @brief  BGP 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/03/04
 */
#include "bgp_bdr.h"

#include <arpa/inet.h>
#include <string.h>

#include "bgp_bmp_db.h"
#include "bgp_cli.h"
#include "bgp_db.h"
#include "bgp_main.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "route.h"

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
    if (afi == 1 && safi == BGP_SAFI_QP)
    {
        return "ipv4-qp";
    }
    if (afi == 2 && safi == BGP_SAFI_QP)
    {
        return "ipv6-qp";
    }
    if (afi == 1 && safi == BGP_SAFI_LABELED)
    {
        return "ipv4-labeled";
    }
    if (afi == 2 && safi == BGP_SAFI_LABELED)
    {
        return "ipv6-labeled";
    }
    return NULL;
}

// ============================================================================
// 内部辅助
// ============================================================================

static gboolean bgp_bdr_is_af_view(const char *view_name)
{
    return view_name &&
           (strcmp(view_name, CLI_VIEW_BGP_AF_IPV4) == 0 || strcmp(view_name, CLI_VIEW_BGP_AF_IPV6) == 0 ||
            strcmp(view_name, CLI_VIEW_BGP_AF_IPV4_QP) == 0 || strcmp(view_name, CLI_VIEW_BGP_AF_IPV6_QP) == 0 ||
            strcmp(view_name, CLI_VIEW_BGP_AF_IPV4_LABELED) == 0 ||
            strcmp(view_name, CLI_VIEW_BGP_AF_IPV6_LABELED) == 0);
}

static uint32_t bgp_bdr_scope_vrf_id(const cli_show_scope_t *scope)
{
    uint32_t vrf_id = BGP_VRF_PUBLIC_ID;

    if (scope)
    {
        (void)cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_BGP_VRF, &vrf_id);
    }

    return vrf_id;
}

static gboolean bgp_bdr_resolve_scoped_af(const cli_show_scope_t *scope, uint32_t *vrf_id_out, int64_t *afi_out,
                                          int64_t *safi_out)
{
    uint32_t afi = 0;
    uint32_t safi = 0;

    if (!scope || !vrf_id_out || !afi_out || !safi_out || !bgp_bdr_is_af_view(scope->view_name))
    {
        return FALSE;
    }

    if (cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_BGP_AFI, &afi) != 0 ||
        cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_BGP_SAFI, &safi) != 0)
    {
        return FALSE;
    }

    *vrf_id_out = bgp_bdr_scope_vrf_id(scope);
    *afi_out = (int64_t)afi;
    *safi_out = (int64_t)safi;
    return afi_safi_to_str(*afi_out, *safi_out) != NULL;
}

static gboolean bgp_bdr_resolve_scoped_bmp_instance(const cli_show_scope_t *scope, char *inst_name,
                                                    size_t inst_name_len)
{
    if (!scope || !inst_name || inst_name_len == 0 || strcmp(scope->view_name, CLI_VIEW_BGP_BMP) != 0)
    {
        return FALSE;
    }

    return cli_ctx_lookup_text(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_BMP_INST_NAME, inst_name, inst_name_len) ==
               0 &&
           inst_name[0] != '\0';
}

// ============================================================================
// 各表配置追加函数
// ============================================================================

/**
 * @brief 追加协议头配置行（bgp <as> / bgp router-id）
 * @return TRUE 表示协议已配置，FALSE 表示无配置
 */
static gboolean bdr_append_protocol(GString *out)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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

static void bdr_append_vrf_row_config(GString *out, db_row_t *row)
{
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

/**
 * @brief 追加 VRF 级配置（router-id）
 */
static void bdr_append_vrf_config(GString *out)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        bdr_append_vrf_row_config(out, result->rows[i]);
    }

    db_result_free(result);
}

static void bdr_append_vrf_config_scoped(GString *out, uint32_t vrf_id)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int(vrf_id)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_result_t *result = NULL;

    if (db_rpc_query(ctx, BGP_TABLE_VRF, NULL, 0, &filter, &result) == ERRCODE_SUCCESS && result)
    {
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            bdr_append_vrf_row_config(out, result->rows[i]);
        }
    }

    db_value_free(&cond.value);
    if (result)
    {
        db_result_free(result);
    }
}

static void bdr_append_session_row(GString *out, db_row_t *row)
{
    const char *ip = db_row_get_text(row, "neighbor_ip", NULL);
    int64_t remote_as = db_row_get_int(row, "remote_as", 0);
    const char *source_if = db_row_get_text(row, "source_interface", "");
    int64_t ebgp_multihop = db_row_get_int(row, "ebgp_multihop", 0);

    if (!ip)
    {
        return;
    }

    g_string_append_printf(out, " neighbor %s as %ld\r\n", ip, remote_as);
    if (source_if && source_if[0] != '\0')
    {
        g_string_append_printf(out, " neighbor %s source-interface %s\r\n", ip, source_if);
    }
    if (ebgp_multihop > 0)
    {
        g_string_append_printf(out, " neighbor %s ebgp-multihop %ld\r\n", ip, ebgp_multihop);
    }
}

/**
 * @brief 追加会话配置行（neighbor <ip> as <as>）
 */
static void bdr_append_sessions(GString *out)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_SESSION, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        bdr_append_session_row(out, result->rows[i]);
    }

    db_result_free(result);
}

static void bdr_append_sessions_scoped(GString *out, uint32_t vrf_id)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int(vrf_id)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_result_t *result = NULL;

    if (db_rpc_query(ctx, BGP_TABLE_SESSION, NULL, 0, &filter, &result) == ERRCODE_SUCCESS && result)
    {
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            bdr_append_session_row(out, result->rows[i]);
        }
    }

    db_value_free(&cond.value);
    if (result)
    {
        db_result_free(result);
    }
}

/**
 * @brief 追加某 AF 下使能邻居配置行（neighbor <ip> enable）
 * @param afi  整数 AFI（与 bgp_neighbor 表存储一致）
 * @param safi 整数 SAFI
 */
static void bdr_append_af_peers(GString *out, int64_t vrf_id, int64_t afi, int64_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int(vrf_id)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int(afi)},
        {.field_name = "safi", .op = DB_CMP_EQ, .value = db_value_int(safi)},
    };
    db_filter_t filter = {.conditions = conds, .num_conditions = G_N_ELEMENTS(conds)};

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_NEIGHBOR, NULL, 0, &filter, &result) != ERRCODE_SUCCESS || !result)
    {
        db_value_free(&conds[0].value);
        db_value_free(&conds[1].value);
        db_value_free(&conds[2].value);
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *ip = db_row_get_text(row, "neighbor_ip", NULL);
        if (!ip)
        {
            continue;
        }
        g_string_append_printf(out, "  neighbor %s enable\r\n", ip);
        if (db_row_get_int(row, "is_rr_client", 0) != 0)
        {
            g_string_append_printf(out, "  neighbor %s reflect-client\r\n", ip);
        }
    }

    db_result_free(result);
    db_value_free(&conds[0].value);
    db_value_free(&conds[1].value);
    db_value_free(&conds[2].value);
}

static void bdr_append_qp_routes(GString *out, int64_t vrf_id, int64_t afi, int64_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int(vrf_id)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int(afi)},
        {.field_name = "safi", .op = DB_CMP_EQ, .value = db_value_int(safi)},
    };
    db_filter_t filter = {.conditions = conds, .num_conditions = G_N_ELEMENTS(conds)};

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_QP_ROUTE, NULL, 0, &filter, &result) != ERRCODE_SUCCESS || !result)
    {
        db_value_free(&conds[0].value);
        db_value_free(&conds[1].value);
        db_value_free(&conds[2].value);
        return;
    }

    const char *ip_kw = (afi == BGP_AFI_IPV6) ? "ipv6" : "ip";
    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        int64_t start_dqpn = db_row_get_int(row, "start_dqpn", 0);
        int64_t route_count = db_row_get_int(row, "route_count", 0);
        int64_t mask_len = db_row_get_int(row, "mask_len", 0);
        const char *prefix_addr = db_row_get_text(row, "prefix_addr", NULL);
        const char *bid = db_row_get_text(row, "bid", NULL);

        if (!prefix_addr || !bid || start_dqpn <= 0 || route_count <= 0 || mask_len <= 0)
        {
            continue;
        }

        g_string_append_printf(out, "  route start-dqpn %ld %s %s mask %ld count %ld bid %s\r\n", start_dqpn, ip_kw,
                               prefix_addr, mask_len, route_count, bid);
    }

    db_result_free(result);
    db_value_free(&conds[0].value);
    db_value_free(&conds[1].value);
    db_value_free(&conds[2].value);
}

/**
 * @brief 追加单个 AF 完整配置块（af <afi> / 各子表配置 / !）
 * @param afi           整数 AFI
 * @param safi          整数 SAFI
 * @param import_protos 已导入协议位掩码
 */
static void bdr_append_af_block(GString *out, int64_t vrf_id, const char *afi_str, int64_t afi, int64_t safi,
                                int64_t import_protos, gboolean route_select_enabled, int64_t cluster_id,
                                int64_t import_rib_sources)
{
    g_string_append(out, " !\r\n");
    g_string_append_printf(out, " af %s\r\n", afi_str);

    /* 反射器 cluster-id（per-AF） */
    if (cluster_id != 0)
    {
        char cid_str[16];
        struct in_addr ia = {.s_addr = htonl((uint32_t)cluster_id)};
        inet_ntop(AF_INET, &ia, cid_str, sizeof(cid_str));
        g_string_append_printf(out, "  reflector cluster-id %s\r\n", cid_str);
    }

    /* AF 下各子表 BDR，按需扩展 */
    bdr_append_af_peers(out, vrf_id, afi, safi);

    /* 导入路由配置 */
    if (import_protos & (1 << ROUTE_PROTOCOL_STATIC))
    {
        g_string_append(out, "  import-route static\r\n");
    }
    if (import_protos & (1 << ROUTE_PROTOCOL_CONNECTED))
    {
        g_string_append(out, "  import-route connected\r\n");
    }

    /* import-rib 跨 AF 路由互导（unicast AF 视图） */
    if (safi == BGP_SAFI_UNICAST && (import_rib_sources & (1 << 0 /* BGP_IMPORT_SRC_LABELED_UC */)))
    {
        g_string_append(out, "  import-rib labeled-unicast\r\n");
    }

    if (safi == BGP_SAFI_QP)
    {
        bdr_append_qp_routes(out, vrf_id, afi, safi);
        if (route_select_enabled)
        {
            g_string_append(out, "  route-select enable\r\n");
        }
    }
}

/**
 * @brief 遍历 bgp_instance 表，对每个 AF 实例输出完整配置块
 */
static void bdr_append_af_instances(GString *out)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
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
        int64_t vrf_id = db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);
        int64_t afi_int = db_row_get_int(row, "afi", 0);
        int64_t safi_int = db_row_get_int(row, "safi", 0);
        int64_t import_protos = db_row_get_int(row, "import_protos", 0);
        gboolean route_select_enabled = db_row_get_int(row, "route_select_enabled", 0) != 0;
        int64_t cluster_id = db_row_get_int(row, "cluster_id", 0);
        int64_t import_rib_sources = db_row_get_int(row, "import_rib_sources", 0);

        const char *afi_str = afi_safi_to_str(afi_int, safi_int);
        if (!afi_str)
        {
            continue;
        }

        bdr_append_af_block(out, vrf_id, afi_str, afi_int, safi_int, import_protos, route_select_enabled, cluster_id,
                            import_rib_sources);
    }

    db_result_free(inst_result);
}

static void bdr_append_af_instances_scoped(GString *out, uint32_t vrf_id)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int(vrf_id)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_result_t *inst_result = NULL;

    if (db_rpc_query(ctx, BGP_TABLE_INSTANCE, NULL, 0, &filter, &inst_result) != ERRCODE_SUCCESS || !inst_result ||
        inst_result->num_rows == 0)
    {
        db_value_free(&cond.value);
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
        int64_t import_protos = db_row_get_int(row, "import_protos", 0);
        gboolean route_select_enabled = db_row_get_int(row, "route_select_enabled", 0) != 0;
        int64_t cluster_id = db_row_get_int(row, "cluster_id", 0);
        int64_t import_rib_sources = db_row_get_int(row, "import_rib_sources", 0);
        const char *afi_str = afi_safi_to_str(afi_int, safi_int);

        if (!afi_str)
        {
            continue;
        }

        bdr_append_af_block(out, vrf_id, afi_str, afi_int, safi_int, import_protos, route_select_enabled, cluster_id,
                            import_rib_sources);
    }

    db_value_free(&cond.value);
    db_result_free(inst_result);
}

static void bdr_append_scoped_af_instance(GString *out, uint32_t vrf_id, int64_t afi, int64_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "vrf_id", .op = DB_CMP_EQ, .value = db_value_int(vrf_id)},
        {.field_name = "afi", .op = DB_CMP_EQ, .value = db_value_int(afi)},
        {.field_name = "safi", .op = DB_CMP_EQ, .value = db_value_int(safi)},
    };
    db_filter_t filter = {.conditions = conds, .num_conditions = G_N_ELEMENTS(conds)};
    db_result_t *result = NULL;

    if (db_rpc_query(ctx, BGP_TABLE_INSTANCE, NULL, 0, &filter, &result) == ERRCODE_SUCCESS && result &&
        result->num_rows > 0)
    {
        db_row_t *row = result->rows[0];
        int64_t import_protos = db_row_get_int(row, "import_protos", 0);
        gboolean route_select_enabled = db_row_get_int(row, "route_select_enabled", 0) != 0;
        int64_t cluster_id = db_row_get_int(row, "cluster_id", 0);
        int64_t import_rib_sources = db_row_get_int(row, "import_rib_sources", 0);
        const char *afi_str = afi_safi_to_str(afi, safi);

        if (afi_str)
        {
            bdr_append_af_block(out, vrf_id, afi_str, afi, safi, import_protos, route_select_enabled, cluster_id,
                                import_rib_sources);
        }
    }

    for (guint i = 0; i < G_N_ELEMENTS(conds); i++)
    {
        db_value_free(&conds[i].value);
    }
    if (result)
    {
        db_result_free(result);
    }
}

// ============================================================================
// BMP 实例配置输出
// ============================================================================

/**
 * @brief 追加单个 BMP 实例的监控邻居配置
 */
static void bdr_append_bmp_monitors(GString *out, const char *inst_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {
        .field_name = "instance_name",
        .op = DB_CMP_EQ,
        .value = db_value_text(inst_name),
    };
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_result_t *result = NULL;

    if (db_rpc_query(ctx, BGP_TABLE_BMP_MONITOR, NULL, 0, &filter, &result) != ERRCODE_SUCCESS || !result)
    {
        db_value_free(&cond.value);
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        const char *ip = db_row_get_text(result->rows[i], "neighbor_ip", NULL);
        if (ip)
        {
            g_string_append_printf(out, "  monitor neighbor %s\r\n", ip);
        }
    }

    db_result_free(result);
    db_value_free(&cond.value);
}

static void bdr_append_bmp_instance_row(GString *out, db_row_t *row)
{
    const char *name = db_row_get_text(row, "instance_name", NULL);
    if (!name)
    {
        return;
    }

    const char *collector_ip = db_row_get_text(row, "collector_ip", "");
    int64_t collector_port = db_row_get_int(row, "collector_port", 0);
    int64_t stats_interval = db_row_get_int(row, "stats_interval", 0);
    int64_t reconnect_interval = db_row_get_int(row, "reconnect_interval", 30);
    int64_t monitor_all = db_row_get_int(row, "monitor_all", 1);

    g_string_append_printf(out, " bmp instance %s\r\n", name);

    if (collector_ip[0] != '\0' && collector_port > 0)
    {
        g_string_append_printf(out, "  collector %s port %ld\r\n", collector_ip, collector_port);
    }

    if (stats_interval > 0)
    {
        g_string_append_printf(out, "  stats-report interval %ld\r\n", stats_interval);
    }

    if (reconnect_interval != 30)
    {
        g_string_append_printf(out, "  reconnect interval %ld\r\n", reconnect_interval);
    }

    if (monitor_all)
    {
        g_string_append(out, "  monitor neighbor all\r\n");
    }
    else
    {
        bdr_append_bmp_monitors(out, name);
    }

    g_string_append(out, " !\r\n");
}

/**
 * @brief 遍历 bgp_bmp_instance 表，输出所有 BMP 实例配置块
 */
static void bdr_append_bmp_instances(GString *out)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_BMP_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result ||
        result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        bdr_append_bmp_instance_row(out, result->rows[i]);
    }

    db_result_free(result);
}

static void bdr_append_scoped_bmp_instance(GString *out, const char *inst_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {.field_name = "instance_name", .op = DB_CMP_EQ, .value = db_value_text(inst_name)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_result_t *result = NULL;

    if (db_rpc_query(ctx, BGP_TABLE_BMP_INSTANCE, NULL, 0, &filter, &result) == ERRCODE_SUCCESS && result &&
        result->num_rows > 0)
    {
        bdr_append_bmp_instance_row(out, result->rows[0]);
    }

    db_value_free(&cond.value);
    if (result)
    {
        db_result_free(result);
    }
}

static void bgp_bdr_show_config_scoped(dev_ipc_message_t *msg, const cli_show_scope_t *scope)
{
    GString *out = g_string_new("");
    if (!out)
    {
        bgp_send_cli_response(msg, "");
        return;
    }

    if (strcmp(scope->view_name, CLI_VIEW_BGP) == 0)
    {
        uint32_t vrf_id = bgp_bdr_scope_vrf_id(scope);
        (void)bdr_append_protocol(out);
        bdr_append_vrf_config_scoped(out, vrf_id);
        bdr_append_sessions_scoped(out, vrf_id);
        bdr_append_af_instances_scoped(out, vrf_id);
        bdr_append_bmp_instances(out);
        g_string_append(out, "!\r\n");
    }
    else if (bgp_bdr_is_af_view(scope->view_name))
    {
        uint32_t vrf_id = BGP_VRF_PUBLIC_ID;
        int64_t afi = 0;
        int64_t safi = 0;

        if (bgp_bdr_resolve_scoped_af(scope, &vrf_id, &afi, &safi))
        {
            bdr_append_scoped_af_instance(out, vrf_id, afi, safi);
        }
    }
    else if (strcmp(scope->view_name, CLI_VIEW_BGP_BMP) == 0)
    {
        char inst_name[32] = {0};
        if (bgp_bdr_resolve_scoped_bmp_instance(scope, inst_name, sizeof(inst_name)))
        {
            bdr_append_scoped_bmp_instance(out, inst_name);
        }
    }

    bgp_send_cli_response(msg, out->str);
    g_string_free(out, TRUE);
}

// ============================================================================
// 公共 API
// ============================================================================

void bgp_bdr_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        LOG_WARN("BGP BDR: invalid SHOW_CONFIG scope payload");
        bgp_send_cli_response(msg, "");
        return;
    }

    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS)
    {
        bgp_bdr_show_config_scoped(msg, &scope);
        return;
    }

    GString *out = g_string_new("");
    if (!out)
    {
        bgp_send_cli_response(msg, "");
        return;
    }

    (void)bdr_append_protocol(out);
    bdr_append_vrf_config(out);
    bdr_append_sessions(out);
    bdr_append_af_instances(out);
    bdr_append_bmp_instances(out);
    g_string_append(out, "!\r\n");

    bgp_send_cli_response(msg, out->str);
    g_string_free(out, TRUE);
}
