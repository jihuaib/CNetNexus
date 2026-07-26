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
#include "vrf.h"

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
    if (afi == 1 && safi == BGP_SAFI_VPN_UNICAST)
    {
        return "vpnv4";
    }
    if (afi == BGP_AFI_L2VPN && safi == BGP_SAFI_EVPN)
    {
        return "evpn";
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
            strcmp(view_name, CLI_VIEW_BGP_AF_IPV4_LABELED) == 0 || strcmp(view_name, CLI_VIEW_BGP_AF_VPNV4) == 0 ||
            strcmp(view_name, CLI_VIEW_BGP_AF_EVPN) == 0 || strcmp(view_name, CLI_VIEW_BGP_VRF_AF_IPV4) == 0 ||
            strcmp(view_name, CLI_VIEW_BGP_VRF_AF_IPV6) == 0);
}

static gboolean bgp_bdr_is_vrf_scoped_view(const char *view_name)
{
    return view_name && (strcmp(view_name, CLI_VIEW_BGP_VRF) == 0 || strcmp(view_name, CLI_VIEW_BGP_VRF_AF_IPV4) == 0 ||
                         strcmp(view_name, CLI_VIEW_BGP_VRF_AF_IPV6) == 0);
}

static gboolean bgp_bdr_resolve_scope_vrf_name(const cli_show_scope_t *scope, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0)
    {
        return FALSE;
    }

    snprintf(buf, buf_len, "%s", VRF_PUBLIC_VRF_NAME);
    if (!scope)
    {
        return TRUE;
    }

    if (cli_ctx_lookup_text(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_VRF_NAME, buf, buf_len) == 0 && buf[0] != '\0')
    {
        return TRUE;
    }

    if (bgp_bdr_is_vrf_scoped_view(scope->view_name))
    {
        buf[0] = '\0';
        return FALSE;
    }

    snprintf(buf, buf_len, "%s", VRF_PUBLIC_VRF_NAME);
    return TRUE;
}

static gboolean bgp_bdr_resolve_scoped_af(const cli_show_scope_t *scope, char *vrf_name, size_t vrf_name_len,
                                          int64_t *afi_out, int64_t *safi_out)
{
    uint32_t afi = 0;
    uint32_t safi = 0;

    if (!scope || !vrf_name || vrf_name_len == 0 || !afi_out || !safi_out || !bgp_bdr_is_af_view(scope->view_name))
    {
        return FALSE;
    }

    if (cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_BGP_AFI, &afi) != 0 ||
        cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_BGP_SAFI, &safi) != 0)
    {
        return FALSE;
    }

    if (!bgp_bdr_resolve_scope_vrf_name(scope, vrf_name, vrf_name_len))
    {
        return FALSE;
    }
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

static void bdr_append_vrf_row_config(GString *out, db_row_t *row, const char *line_indent)
{
    const char *router_id = db_row_get_text(row, "router_id", NULL);
    int64_t keepalive = db_row_get_int(row, "keepalive", BGP_TIMER_DEFAULT_KEEPALIVE);
    int64_t hold_time = db_row_get_int(row, "hold_time", BGP_TIMER_DEFAULT_HOLD);
    int64_t connect_retry = db_row_get_int(row, "connect_retry", BGP_TIMER_DEFAULT_CONNECT_RETRY);

    if (router_id && strcmp(router_id, "0.0.0.0") != 0)
    {
        g_string_append_printf(out, "%srouter-id %s\r\n", line_indent, router_id);
    }

    if (keepalive != BGP_TIMER_DEFAULT_KEEPALIVE || hold_time != BGP_TIMER_DEFAULT_HOLD)
    {
        g_string_append_printf(out, "%stimer keepalive %ld hold %ld\r\n", line_indent, keepalive, hold_time);
    }

    if (connect_retry != BGP_TIMER_DEFAULT_CONNECT_RETRY)
    {
        g_string_append_printf(out, "%stimer connect-retry %ld\r\n", line_indent, connect_retry);
    }
}

static void bdr_append_vrf_config_scoped(GString *out, const char *vrf_name, const char *line_indent)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {.field_name = "vrf_name", .op = DB_CMP_EQ, .value = db_value_text(vrf_name)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_result_t *result = NULL;

    if (db_rpc_query(ctx, BGP_TABLE_VRF, NULL, 0, &filter, &result) == ERRCODE_SUCCESS && result)
    {
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            bdr_append_vrf_row_config(out, result->rows[i], line_indent);
        }
    }

    db_value_free(&cond.value);
    if (result)
    {
        db_result_free(result);
    }
}

static void bdr_append_session_row(GString *out, db_row_t *row, const char *line_indent)
{
    const char *ip = db_row_get_text(row, "neighbor_ip", NULL);
    int64_t remote_as = db_row_get_int(row, "remote_as", 0);
    const char *source_if = db_row_get_text(row, "source_interface", "");
    int64_t ebgp_multihop = db_row_get_int(row, "ebgp_multihop", 0);

    if (!ip)
    {
        return;
    }

    g_string_append_printf(out, "%sneighbor %s as %ld\r\n", line_indent, ip, remote_as);
    if (source_if && source_if[0] != '\0')
    {
        g_string_append_printf(out, "%sneighbor %s source-interface %s\r\n", line_indent, ip, source_if);
    }
    if (ebgp_multihop > 0)
    {
        g_string_append_printf(out, "%sneighbor %s ebgp-multihop %ld\r\n", line_indent, ip, ebgp_multihop);
    }
}

static void bdr_append_sessions_scoped(GString *out, const char *vrf_name, const char *line_indent)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {.field_name = "vrf_name", .op = DB_CMP_EQ, .value = db_value_text(vrf_name)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_result_t *result = NULL;

    if (db_rpc_query(ctx, BGP_TABLE_SESSION, NULL, 0, &filter, &result) == ERRCODE_SUCCESS && result)
    {
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            bdr_append_session_row(out, result->rows[i], line_indent);
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
static void bdr_append_af_peers(GString *out, const char *vrf_name, int64_t afi, int64_t safi, const char *line_indent)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "vrf_name", .op = DB_CMP_EQ, .value = db_value_text(vrf_name)},
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
        g_string_append_printf(out, "%sneighbor %s enable\r\n", line_indent, ip);
        if (db_row_get_int(row, "is_rr_client", 0) != 0)
        {
            g_string_append_printf(out, "%sneighbor %s reflect-client\r\n", line_indent, ip);
        }
        const char *export_policy = db_row_get_text(row, "export_policy", "");
        if (export_policy && export_policy[0] != '\0')
        {
            g_string_append_printf(out, "%sneighbor %s route-policy %s export\r\n", line_indent, ip, export_policy);
        }
    }

    db_result_free(result);
    db_value_free(&conds[0].value);
    db_value_free(&conds[1].value);
    db_value_free(&conds[2].value);
}

static void bdr_append_qp_routes(GString *out, const char *vrf_name, int64_t afi, int64_t safi, const char *line_indent)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "vrf_name", .op = DB_CMP_EQ, .value = db_value_text(vrf_name)},
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

        g_string_append_printf(out, "%sroute start-dqpn %ld %s %s mask %ld count %ld bid %s\r\n", line_indent,
                               start_dqpn, ip_kw, prefix_addr, mask_len, route_count, bid);
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
static void bdr_append_af_block(GString *out, const char *vrf_name, const char *afi_str, int64_t afi, int64_t safi,
                                int64_t import_protos, gboolean route_select_enabled, int64_t cluster_id,
                                int64_t import_rib_sources, gboolean vpn_target_policy, gboolean advertise_evpn_route,
                                const char *block_indent, const char *body_indent)
{
    g_string_append_printf(out, "%s!\r\n", block_indent);
    g_string_append_printf(out, "%saf %s\r\n", block_indent, afi_str);

    /* VPN AF 入向过滤策略：默认启用，仅关闭(no policy vpn-target)时进 running-config */
    if (safi == BGP_SAFI_VPN_UNICAST && !vpn_target_policy)
    {
        g_string_append_printf(out, "%sno policy vpn-target\r\n", body_indent);
    }

    /* 反射器 cluster-id（per-AF） */
    if (cluster_id != 0)
    {
        char cid_str[16];
        struct in_addr ia = {.s_addr = htonl((uint32_t)cluster_id)};
        inet_ntop(AF_INET, &ia, cid_str, sizeof(cid_str));
        g_string_append_printf(out, "%sreflector cluster-id %s\r\n", body_indent, cid_str);
    }

    /* AF 下各子表 BDR，按需扩展 */
    bdr_append_af_peers(out, vrf_name, afi, safi, body_indent);

    /* 导入路由配置 */
    if (import_protos & (1 << ROUTE_PROTOCOL_STATIC))
    {
        g_string_append_printf(out, "%simport-route static\r\n", body_indent);
    }
    if (import_protos & (1 << ROUTE_PROTOCOL_CONNECTED))
    {
        g_string_append_printf(out, "%simport-route connected\r\n", body_indent);
    }

    /* import 跨 AF 路由互导（仅 IPv4 unicast AF 视图） */
    if (afi == BGP_AFI_IPV4 && safi == BGP_SAFI_UNICAST &&
        (import_rib_sources & (1 << 0 /* BGP_IMPORT_SRC_LABELED_UC */)))
    {
        g_string_append_printf(out, "%simport-rib public ipv4-labeled-unicast\r\n", body_indent);
    }

    if (afi == BGP_AFI_IPV4 && safi == BGP_SAFI_UNICAST && strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) != 0 &&
        advertise_evpn_route)
    {
        g_string_append_printf(out, "%sadvertise evpn route\r\n", body_indent);
    }

    if (safi == BGP_SAFI_QP)
    {
        bdr_append_qp_routes(out, vrf_name, afi, safi, body_indent);
        if (route_select_enabled)
        {
            g_string_append_printf(out, "%sroute-select enable\r\n", body_indent);
        }
    }
}

static void bdr_append_af_instances_scoped(GString *out, const char *vrf_name, const char *block_indent,
                                           const char *body_indent)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {.field_name = "vrf_name", .op = DB_CMP_EQ, .value = db_value_text(vrf_name)};
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
        gboolean vpn_target_policy = db_row_get_int(row, "vpn_target_policy", 1) != 0;
        gboolean advertise_evpn_route = db_row_get_int(row, "advertise_evpn_route", 0) != 0;
        const char *afi_str = afi_safi_to_str(afi_int, safi_int);

        if (!afi_str)
        {
            continue;
        }

        bdr_append_af_block(out, vrf_name, afi_str, afi_int, safi_int, import_protos, route_select_enabled, cluster_id,
                            import_rib_sources, vpn_target_policy, advertise_evpn_route, block_indent, body_indent);
    }

    db_value_free(&cond.value);
    db_result_free(inst_result);
}

static void bdr_collect_vrf_names_from_table(GHashTable *names, const char *table_name)
{
    if (!names || !table_name)
    {
        return;
    }

    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, table_name, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        const char *vrf_name = db_row_get_text(result->rows[i], "vrf_name", VRF_PUBLIC_VRF_NAME);
        if (vrf_name && strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) != 0)
        {
            g_hash_table_add(names, g_strdup(vrf_name));
        }
    }

    db_result_free(result);
}

static gint bdr_compare_vrf_name(gconstpointer a, gconstpointer b)
{
    return g_strcmp0((const char *)a, (const char *)b);
}

static void bdr_append_non_public_vrf_blocks(GString *out)
{
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!names)
    {
        return;
    }

    bdr_collect_vrf_names_from_table(names, BGP_TABLE_VRF);
    bdr_collect_vrf_names_from_table(names, BGP_TABLE_SESSION);
    bdr_collect_vrf_names_from_table(names, BGP_TABLE_INSTANCE);

    GList *keys = g_hash_table_get_keys(names);
    keys = g_list_sort(keys, bdr_compare_vrf_name);

    for (GList *l = keys; l; l = l->next)
    {
        const char *vrf_name = (const char *)l->data;
        if (!vrf_name || vrf_name[0] == '\0')
        {
            continue;
        }

        g_string_append(out, " !\r\n");
        g_string_append_printf(out, " vrf %s\r\n", vrf_name);
        bdr_append_vrf_config_scoped(out, vrf_name, "  ");
        bdr_append_sessions_scoped(out, vrf_name, "  ");
        bdr_append_af_instances_scoped(out, vrf_name, "  ", "   ");
        g_string_append(out, " !\r\n");
    }

    g_list_free(keys);
    g_hash_table_destroy(names);
}

static void bdr_append_scoped_af_instance(GString *out, const char *vrf_name, int64_t afi, int64_t safi,
                                          const char *block_indent, const char *body_indent)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "vrf_name", .op = DB_CMP_EQ, .value = db_value_text(vrf_name)},
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
        gboolean vpn_target_policy = db_row_get_int(row, "vpn_target_policy", 1) != 0;
        gboolean advertise_evpn_route = db_row_get_int(row, "advertise_evpn_route", 0) != 0;
        const char *afi_str = afi_safi_to_str(afi, safi);

        if (afi_str)
        {
            bdr_append_af_block(out, vrf_name, afi_str, afi, safi, import_protos, route_select_enabled, cluster_id,
                                import_rib_sources, vpn_target_policy, advertise_evpn_route, block_indent, body_indent);
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

    if (strcmp(scope->view_name, CLI_VIEW_BGP) == 0 || strcmp(scope->view_name, CLI_VIEW_BGP_VRF) == 0)
    {
        char vrf_name[VRF_NAME_MAX_LEN];
        if (!bgp_bdr_resolve_scope_vrf_name(scope, vrf_name, sizeof(vrf_name)))
        {
            bgp_send_cli_response(msg, out->str);
            g_string_free(out, TRUE);
            return;
        }

        if (strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
        {
            (void)bdr_append_protocol(out);
            bdr_append_vrf_config_scoped(out, vrf_name, " ");
            bdr_append_sessions_scoped(out, vrf_name, " ");
            bdr_append_af_instances_scoped(out, vrf_name, " ", "  ");
            bdr_append_non_public_vrf_blocks(out);
            bdr_append_bmp_instances(out);
        }
        else
        {
            g_string_append(out, " !\r\n");
            g_string_append_printf(out, " vrf %s\r\n", vrf_name);
            bdr_append_vrf_config_scoped(out, vrf_name, "  ");
            bdr_append_sessions_scoped(out, vrf_name, "  ");
            bdr_append_af_instances_scoped(out, vrf_name, "  ", "   ");
            g_string_append(out, " !\r\n");
        }
        if (out->len > 0)
        {
            g_string_append(out, "!\r\n");
        }
    }
    else if (bgp_bdr_is_af_view(scope->view_name))
    {
        char vrf_name[VRF_NAME_MAX_LEN];
        int64_t afi = 0;
        int64_t safi = 0;

        if (bgp_bdr_resolve_scoped_af(scope, vrf_name, sizeof(vrf_name), &afi, &safi))
        {
            gboolean vrf_scoped = bgp_bdr_is_vrf_scoped_view(scope->view_name);
            bdr_append_scoped_af_instance(out, vrf_name, afi, safi, vrf_scoped ? "  " : " ", vrf_scoped ? "   " : "  ");
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
    bdr_append_vrf_config_scoped(out, VRF_PUBLIC_VRF_NAME, " ");
    bdr_append_sessions_scoped(out, VRF_PUBLIC_VRF_NAME, " ");
    bdr_append_af_instances_scoped(out, VRF_PUBLIC_VRF_NAME, " ", "  ");
    bdr_append_non_public_vrf_blocks(out);
    bdr_append_bmp_instances(out);
    if (out->len > 0)
    {
        g_string_append(out, "!\r\n");
    }

    /*
     * BGP 出口策略引用依赖 RPM 策略已经创建。完整配置通过 anchor 延后到
     * 全局配置段之后渲染，使 module-id 更大的 RPM 仍能先于 BGP 回放；
     * show this 保持上面的普通文本输出。
     */
    GString *wrapped = g_string_new("");
    cli_cfg_anchor_emit_header(wrapped, "bgp/global", out->str);
    bgp_send_cli_response(msg, wrapped->str);
    g_string_free(wrapped, TRUE);
    g_string_free(out, TRUE);
}
