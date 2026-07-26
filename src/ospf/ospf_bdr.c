/**
 * @file   ospf_bdr.c
 * @brief  OSPFv2 running-configuration builder
 */

#include "ospf_bdr.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "log.h"
#include "ospf.h"
#include "ospf_db.h"
#include "ospf_main.h"

static cli_chunk_stream_t g_ospf_bdr_stream;

static void ospf_bdr_send_resp(dev_ipc_message_t *msg, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_OSPF, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1u, g_free);
    if (resp)
    {
        dev_ipc_send_response(ospf_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static int ospf_bdr_send_stream(dev_ipc_message_t *msg, GString *out)
{
    return cli_chunk_stream_start(&g_ospf_bdr_stream, ospf_local_ipc_ctx(), DEV_MODULE_ID_OSPF, msg, out);
}

static gboolean ospf_bdr_int_is_u32(int64_t value)
{
    return value >= 0 && (uint64_t)value <= UINT32_MAX;
}

static gint ospf_bdr_compare_u32(gconstpointer a, gconstpointer b)
{
    const uint32_t lhs = *(const uint32_t *)a;
    const uint32_t rhs = *(const uint32_t *)b;
    return (lhs > rhs) - (lhs < rhs);
}

static void ospf_bdr_append_areas(GString *out, dev_ipc_context_t *ctx, uint32_t process_id)
{
    db_filter_builder_t filter;
    db_filter_init(&filter);
    db_filter_add_int(&filter, "process_id", (int64_t)process_id);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, OSPF_TABLE_AREA, NULL, 0, &filter.filter, &result);
    db_filter_clear(&filter);
    if (rc != ERRCODE_SUCCESS || !result)
    {
        if (result)
        {
            db_result_free(result);
        }
        return;
    }

    GArray *areas = g_array_sized_new(FALSE, FALSE, sizeof(uint32_t), result->num_rows);
    if (areas)
    {
        for (uint32_t i = 0u; i < result->num_rows; ++i)
        {
            int64_t area_id = db_row_get_int(result->rows[i], "area_id", -1);
            if (ospf_bdr_int_is_u32(area_id))
            {
                uint32_t value = (uint32_t)area_id;
                g_array_append_val(areas, value);
            }
        }
        g_array_sort(areas, ospf_bdr_compare_u32);

        uint32_t previous = 0u;
        gboolean have_previous = FALSE;
        for (guint i = 0u; i < areas->len; ++i)
        {
            uint32_t area_id = g_array_index(areas, uint32_t, i);
            if (!have_previous || area_id != previous)
            {
                g_string_append_printf(out, " area %u\r\n", area_id);
                previous = area_id;
                have_previous = TRUE;
            }
        }
        g_array_free(areas, TRUE);
    }
    db_result_free(result);
}

static void ospf_bdr_if_cfg_from_row(const db_row_t *row, ospf_if_cfg_t *cfg)
{
    const char *ifname = db_row_get_text(row, "ifname", "");
    int64_t area_id = db_row_get_int(row, "area_id", OSPF_AREA_BACKBONE);
    int64_t network_type = db_row_get_int(row, "network_type", OSPF_NETWORK_BROADCAST);
    int64_t cost = db_row_get_int(row, "cost", OSPF_DEFAULT_COST);
    int64_t hello_interval = db_row_get_int(row, "hello_interval", OSPF_DEFAULT_HELLO_INTERVAL);
    int64_t dead_interval = db_row_get_int(row, "dead_interval", OSPF_DEFAULT_DEAD_INTERVAL);
    int64_t priority = db_row_get_int(row, "priority", OSPF_DEFAULT_PRIORITY);

    memset(cfg, 0, sizeof(*cfg));
    g_strlcpy(cfg->ifname, ifname ? ifname : "", sizeof(cfg->ifname));
    cfg->enabled = db_row_get_int(row, "enabled", 1) ? 1u : 0u;
    cfg->passive = db_row_get_int(row, "passive", 0) ? 1u : 0u;
    cfg->area_id = ospf_bdr_int_is_u32(area_id) ? (uint32_t)area_id : OSPF_AREA_BACKBONE;
    cfg->network_type =
        (network_type == OSPF_NETWORK_POINT_TO_POINT) ? OSPF_NETWORK_POINT_TO_POINT : OSPF_NETWORK_BROADCAST;
    cfg->cost = (cost > 0 && cost <= OSPF_MAX_COST) ? (uint16_t)cost : OSPF_DEFAULT_COST;
    cfg->hello_interval =
        (hello_interval > 0 && hello_interval <= UINT16_MAX) ? (uint16_t)hello_interval : OSPF_DEFAULT_HELLO_INTERVAL;
    cfg->dead_interval = (dead_interval > 0 && (uint64_t)dead_interval <= UINT32_MAX) ? (uint32_t)dead_interval
                                                                                      : OSPF_DEFAULT_DEAD_INTERVAL;
    cfg->priority = (priority >= 0 && priority <= UINT8_MAX) ? (uint8_t)priority : OSPF_DEFAULT_PRIORITY;
}

static void ospf_bdr_append_instance(GString *out, dev_ipc_context_t *ctx, const db_row_t *row)
{
    int64_t process_id_value = db_row_get_int(row, "process_id", 0);
    int64_t router_id_value = db_row_get_int(row, "router_id", 0);
    const char *vrf_name = db_row_get_text(row, "vrf_name", "public");
    if (process_id_value <= 0 || !ospf_bdr_int_is_u32(process_id_value))
    {
        return;
    }

    g_string_append(out, "!\r\n");
    g_string_append_printf(out, "ospf %u", (uint32_t)process_id_value);
    if (vrf_name && vrf_name[0] != '\0' && strcmp(vrf_name, "public") != 0)
    {
        g_string_append_printf(out, " vrf %s", vrf_name);
    }
    g_string_append(out, "\r\n");

    if (router_id_value > 0 && ospf_bdr_int_is_u32(router_id_value))
    {
        struct in_addr router_addr = {
            .s_addr = htonl((uint32_t)router_id_value),
        };
        char router_id[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &router_addr, router_id, sizeof(router_id)))
        {
            g_string_append_printf(out, " router-id %s\r\n", router_id);
        }
    }
    ospf_bdr_append_areas(out, ctx, (uint32_t)process_id_value);
    g_string_append(out, "!\r\n");
}

static void ospf_bdr_append_interface(GString *out, uint32_t process_id, const ospf_if_cfg_t *cfg)
{
    if (!out || process_id == 0u || !cfg || cfg->ifname[0] == '\0')
    {
        return;
    }

    GString *body = g_string_new("");
    if (!body)
    {
        return;
    }

    if (cfg->enabled)
    {
        g_string_append_printf(body, " ospf enable %u area %u\r\n", process_id, cfg->area_id);
    }
    if (cfg->cost != OSPF_DEFAULT_COST)
    {
        g_string_append_printf(body, " ospf cost %u %u\r\n", process_id, (unsigned)cfg->cost);
    }
    if (cfg->hello_interval != OSPF_DEFAULT_HELLO_INTERVAL)
    {
        g_string_append_printf(body, " ospf hello-interval %u %u\r\n", process_id, (unsigned)cfg->hello_interval);
    }
    if (cfg->dead_interval != OSPF_DEFAULT_DEAD_INTERVAL)
    {
        g_string_append_printf(body, " ospf dead-interval %u %u\r\n", process_id, cfg->dead_interval);
    }
    if (cfg->priority != OSPF_DEFAULT_PRIORITY)
    {
        g_string_append_printf(body, " ospf priority %u %u\r\n", process_id, (unsigned)cfg->priority);
    }
    if (cfg->network_type == OSPF_NETWORK_POINT_TO_POINT)
    {
        g_string_append_printf(body, " ospf network-type %u point-to-point\r\n", process_id);
    }
    if (cfg->passive)
    {
        g_string_append_printf(body, " ospf passive %u\r\n", process_id);
    }

    if (body->len > 0u)
    {
        char key[CLI_CFG_ANCHOR_KEY_MAX];
        snprintf(key, sizeof(key), "iface/%s", cfg->ifname);
        cli_cfg_anchor_emit_body(out, key, body->str);
    }
    g_string_free(body, TRUE);
}

static void ospf_bdr_append_interfaces(GString *out, dev_ipc_context_t *ctx, uint32_t process_id,
                                       const char *ifname_filter)
{
    db_filter_builder_t filter;
    db_filter_init(&filter);
    if (process_id != 0u)
    {
        db_filter_add_int(&filter, "process_id", (int64_t)process_id);
    }
    if (ifname_filter && ifname_filter[0] != '\0')
    {
        db_filter_add_text(&filter, "ifname", ifname_filter);
    }

    db_result_t *result = NULL;
    const db_filter_t *where = filter.n > 0u ? &filter.filter : NULL;
    int rc = db_rpc_query(ctx, OSPF_TABLE_INTERFACE, NULL, 0, where, &result);
    db_filter_clear(&filter);
    if (rc != ERRCODE_SUCCESS || !result)
    {
        if (result)
        {
            db_result_free(result);
        }
        return;
    }

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (uint32_t i = 0u; i < result->num_rows; ++i)
    {
        db_row_t *row = result->rows[i];
        int64_t row_process_id = db_row_get_int(row, "process_id", 0);
        const char *ifname = db_row_get_text(row, "ifname", NULL);
        if (row_process_id <= 0 || !ospf_bdr_int_is_u32(row_process_id) || !ifname || ifname[0] == '\0')
        {
            continue;
        }

        char dedup_key[IF_LOGICAL_NAME_MAX + 32u];
        g_snprintf(dedup_key, sizeof(dedup_key), "%u|%s", (uint32_t)row_process_id, ifname);
        if (seen && g_hash_table_contains(seen, dedup_key))
        {
            continue;
        }
        if (seen)
        {
            g_hash_table_add(seen, g_strdup(dedup_key));
        }

        ospf_if_cfg_t cfg;
        ospf_bdr_if_cfg_from_row(row, &cfg);
        ospf_bdr_append_interface(out, (uint32_t)row_process_id, &cfg);
    }

    if (seen)
    {
        g_hash_table_destroy(seen);
    }
    db_result_free(result);
}

static const char *ospf_bdr_if_ctx_idx_to_name(uint32_t if_idx)
{
    switch (if_idx)
    {
        case 0:
            return "null0";
        case 1:
            return "GE-1";
        case 2:
            return "GE-2";
        case 3:
            return "GE-3";
        case 4:
            return "GE-4";
        case 5:
            return "GE-5";
        case 6:
            return "GE-6";
        case 7:
            return "GE-7";
        case 8:
            return "GE-8";
        default:
            return NULL;
    }
}

static gboolean ospf_bdr_resolve_scoped_ifname(const cli_show_scope_t *scope, char *ifname, size_t ifname_len)
{
    if (!scope || !ifname || ifname_len == 0u)
    {
        return FALSE;
    }

    if (strcmp(scope->view_name, CLI_VIEW_IF) == 0 || strcmp(scope->view_name, CLI_VIEW_IF_NULL0) == 0)
    {
        uint32_t if_idx = 0u;
        if (cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_IF_IDX, &if_idx) != 0)
        {
            return FALSE;
        }
        const char *name = ospf_bdr_if_ctx_idx_to_name(if_idx);
        if (!name)
        {
            return FALSE;
        }
        g_strlcpy(ifname, name, ifname_len);
        return TRUE;
    }

    if (strcmp(scope->view_name, CLI_VIEW_IF_LOOP) == 0)
    {
        uint32_t loop_id = 0u;
        if (cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_IF_LOOP_IDX, &loop_id) != 0 ||
            loop_id == 0u)
        {
            return FALSE;
        }
        g_snprintf(ifname, ifname_len, "loop%u", loop_id);
        return TRUE;
    }

    return FALSE;
}

static gboolean ospf_bdr_resolve_scoped_process(const cli_show_scope_t *scope, uint32_t *process_id_out)
{
    uint32_t process_id = 0u;
    if (!scope || !process_id_out || strcmp(scope->view_name, CLI_VIEW_OSPF) != 0 ||
        cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_OSPF_PROCESS, &process_id) != 0 ||
        process_id == 0u)
    {
        return FALSE;
    }

    *process_id_out = process_id;
    return TRUE;
}

static int ospf_bdr_show_full(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    GString *out = g_string_new("");
    if (!ctx || !out)
    {
        ospf_bdr_send_resp(msg, "");
        if (out)
        {
            g_string_free(out, TRUE);
        }
        return ERRCODE_FAIL;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, OSPF_TABLE_INSTANCE, NULL, 0, NULL, &result) == ERRCODE_SUCCESS && result)
    {
        for (uint32_t i = 0u; i < result->num_rows; ++i)
        {
            db_row_t *row = result->rows[i];
            int64_t process_id = db_row_get_int(row, "process_id", 0);
            if (process_id <= 0 || !ospf_bdr_int_is_u32(process_id))
            {
                continue;
            }
            ospf_bdr_append_instance(out, ctx, row);
            ospf_bdr_append_interfaces(out, ctx, (uint32_t)process_id, NULL);
        }
        db_result_free(result);
    }

    return ospf_bdr_send_stream(msg, out);
}

static int ospf_bdr_show_process(dev_ipc_message_t *msg, const cli_show_scope_t *scope)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    GString *out = g_string_new("");
    uint32_t process_id = 0u;
    if (!ctx || !out || !ospf_bdr_resolve_scoped_process(scope, &process_id))
    {
        ospf_bdr_send_resp(msg, "");
        if (out)
        {
            g_string_free(out, TRUE);
        }
        return ctx && out ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    db_filter_builder_t filter;
    db_filter_init(&filter);
    db_filter_add_int(&filter, "process_id", (int64_t)process_id);
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, OSPF_TABLE_INSTANCE, NULL, 0, &filter.filter, &result) == ERRCODE_SUCCESS && result &&
        result->num_rows > 0u)
    {
        ospf_bdr_append_instance(out, ctx, result->rows[0]);
    }
    db_filter_clear(&filter);
    if (result)
    {
        db_result_free(result);
    }

    return ospf_bdr_send_stream(msg, out);
}

static int ospf_bdr_show_interface(dev_ipc_message_t *msg, const cli_show_scope_t *scope)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    GString *out = g_string_new("");
    char ifname[IF_LOGICAL_NAME_MAX];
    if (!ctx || !out || !ospf_bdr_resolve_scoped_ifname(scope, ifname, sizeof(ifname)))
    {
        ospf_bdr_send_resp(msg, "");
        if (out)
        {
            g_string_free(out, TRUE);
        }
        return ctx && out ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    ospf_bdr_append_interfaces(out, ctx, 0u, ifname);
    return ospf_bdr_send_stream(msg, out);
}

int ospf_bdr_handle_show_config(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        LOG_WARN("OSPF BDR: invalid SHOW_CONFIG scope payload");
        ospf_bdr_send_resp(msg, "");
        return ERRCODE_FAIL;
    }

    if (scope.mode != CLI_SHOW_SCOPE_MODE_THIS)
    {
        return ospf_bdr_show_full(msg);
    }
    if (strcmp(scope.view_name, CLI_VIEW_OSPF) == 0)
    {
        return ospf_bdr_show_process(msg, &scope);
    }
    if (strcmp(scope.view_name, CLI_VIEW_IF) == 0 || strcmp(scope.view_name, CLI_VIEW_IF_NULL0) == 0 ||
        strcmp(scope.view_name, CLI_VIEW_IF_LOOP) == 0)
    {
        return ospf_bdr_show_interface(msg, &scope);
    }

    ospf_bdr_send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

int ospf_bdr_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_ospf_bdr_stream, ospf_local_ipc_ctx(), DEV_MODULE_ID_OSPF, msg);
}

gboolean ospf_bdr_stream_active(void)
{
    return g_ospf_bdr_stream.full_text != NULL;
}

void ospf_bdr_cleanup(void)
{
    cli_chunk_stream_reset(&g_ospf_bdr_stream);
}
