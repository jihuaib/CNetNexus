/**
 * @file   isis_bdr.c
 * @brief  ISIS 配置构建器（show current-configuration）
 * @author jhb
 * @date   2026/04/11
 */
#include "isis_bdr.h"

#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "isis.h"
#include "isis_db.h"
#include "isis_main.h"
#include "log.h"

static void isis_bdr_send_resp(dev_ipc_message_t *msg, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_ISIS, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(isis_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static const char *is_type_to_text(int64_t is_type)
{
    switch ((uint8_t)is_type)
    {
        case ISIS_IS_TYPE_LEVEL_1:
            return "level-1";
        case ISIS_IS_TYPE_LEVEL_2:
            return "level-2";
        default:
            return "level-1-2";
    }
}

static void isis_bdr_if_cfg_defaults(isis_if_cfg_t *cfg, const char *ifname)
{
    if (!cfg)
    {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    if (ifname)
    {
        g_strlcpy(cfg->ifname, ifname, sizeof(cfg->ifname));
    }

    cfg->v4.enabled = 0u;
    cfg->v4.metric = ISIS_DEFAULT_IF_METRIC;
    cfg->v4.hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
    cfg->v4.hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
    cfg->v4.passive = ISIS_DEFAULT_IF_PASSIVE;

    cfg->v6.enabled = 0u;
    cfg->v6.metric = ISIS_DEFAULT_IF_METRIC;
    cfg->v6.hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
    cfg->v6.hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
    cfg->v6.passive = ISIS_DEFAULT_IF_PASSIVE;
}

static void isis_bdr_set_af_cfg_from_row(isis_if_af_cfg_t *af_cfg, db_row_t *if_row)
{
    if (!af_cfg || !if_row)
    {
        return;
    }

    int64_t enabled_i64 = db_row_get_int(if_row, "enabled", 1);
    int64_t metric_i64 = db_row_get_int(if_row, "metric", ISIS_DEFAULT_IF_METRIC);
    int64_t hello_i64 = db_row_get_int(if_row, "hello_interval", ISIS_DEFAULT_HELLO_INTERVAL);
    int64_t hold_i64 = db_row_get_int(if_row, "hold_multiplier", ISIS_DEFAULT_HOLD_MULTIPLIER);
    int64_t passive_i64 = db_row_get_int(if_row, "passive", ISIS_DEFAULT_IF_PASSIVE);

    af_cfg->enabled = enabled_i64 ? 1u : 0u;
    af_cfg->metric =
        (metric_i64 > 0 && metric_i64 <= ISIS_MAX_IF_METRIC) ? (uint32_t)metric_i64 : ISIS_DEFAULT_IF_METRIC;
    af_cfg->hello_interval =
        (hello_i64 > 0 && hello_i64 <= ISIS_MAX_HELLO_INTERVAL) ? (uint16_t)hello_i64 : ISIS_DEFAULT_HELLO_INTERVAL;
    af_cfg->hold_multiplier =
        (hold_i64 > 0 && hold_i64 <= ISIS_MAX_HOLD_MULTIPLIER) ? (uint8_t)hold_i64 : ISIS_DEFAULT_HOLD_MULTIPLIER;
    af_cfg->passive = passive_i64 ? 1u : 0u;
}

/**
 * @brief 作为"接口" anchor 的贡献者, 向 key "iface/<ifname>" 追加 ISIS 行
 *        IF 模块声明接口 anchor 的段头段尾, ISIS 不负责包裹; 聚合器会合并
 *        同一 ifname 的所有贡献。
 */
static void append_if_entry(GString *out, uint32_t tag, const isis_if_cfg_t *cfg)
{
    if (!out || !cfg || cfg->ifname[0] == '\0' || tag == 0u || (!cfg->v4.enabled && !cfg->v6.enabled))
    {
        return;
    }

    GString *body = g_string_new("");

    if (cfg->v4.enabled)
    {
        g_string_append_printf(body, " isis enable %u\r\n", tag);
    }
    if (cfg->v6.enabled)
    {
        g_string_append_printf(body, " isis ipv6 enable %u\r\n", tag);
    }

    if (cfg->v4.enabled && cfg->v4.metric != ISIS_DEFAULT_IF_METRIC)
    {
        g_string_append_printf(body, " isis metric %u %u\r\n", tag, cfg->v4.metric);
    }
    if (cfg->v6.enabled && cfg->v6.metric != ISIS_DEFAULT_IF_METRIC)
    {
        g_string_append_printf(body, " isis ipv6 metric %u %u\r\n", tag, cfg->v6.metric);
    }
    if (cfg->v4.enabled && cfg->v4.hello_interval != ISIS_DEFAULT_HELLO_INTERVAL)
    {
        g_string_append_printf(body, " isis hello-interval %u %u\r\n", tag, (unsigned)cfg->v4.hello_interval);
    }
    if (cfg->v6.enabled && cfg->v6.hello_interval != ISIS_DEFAULT_HELLO_INTERVAL)
    {
        g_string_append_printf(body, " isis ipv6 hello-interval %u %u\r\n", tag, (unsigned)cfg->v6.hello_interval);
    }

    if (cfg->v4.enabled && cfg->v4.hold_multiplier != ISIS_DEFAULT_HOLD_MULTIPLIER)
    {
        g_string_append_printf(body, " isis hold-multiplier %u %u\r\n", tag, (unsigned)cfg->v4.hold_multiplier);
    }
    if (cfg->v6.enabled && cfg->v6.hold_multiplier != ISIS_DEFAULT_HOLD_MULTIPLIER)
    {
        g_string_append_printf(body, " isis ipv6 hold-multiplier %u %u\r\n", tag, (unsigned)cfg->v6.hold_multiplier);
    }

    if (cfg->v4.enabled && cfg->v4.passive)
    {
        g_string_append_printf(body, " isis passive %u\r\n", tag);
    }
    if (cfg->v6.enabled && cfg->v6.passive)
    {
        g_string_append_printf(body, " isis ipv6 passive %u\r\n", tag);
    }

    if (body->len > 0)
    {
        char key[CLI_CFG_ANCHOR_KEY_MAX];
        snprintf(key, sizeof(key), "iface/%s", cfg->ifname);
        cli_cfg_anchor_emit_body(out, key, body->str);
    }
    g_string_free(body, TRUE);
}

static const char *isis_bdr_if_ctx_idx_to_name(uint32_t if_idx)
{
    switch (if_idx)
    {
        case 1:
            return "GE-1";
        case 2:
            return "GE-2";
        case 3:
            return "GE-3";
        case 4:
            return "GE-4";
        default:
            return NULL;
    }
}

static gboolean isis_bdr_resolve_scoped_ifname(const cli_show_scope_t *scope, char *ifname, size_t ifname_len)
{
    if (!scope || !ifname || ifname_len == 0)
    {
        return FALSE;
    }

    if (strcmp(scope->view_name, CLI_VIEW_IF) == 0 || strcmp(scope->view_name, CLI_VIEW_IF_NULL0) == 0)
    {
        uint32_t if_idx = 0;
        if (cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_IF_IDX, &if_idx) != 0)
        {
            return FALSE;
        }

        const char *name = isis_bdr_if_ctx_idx_to_name(if_idx);
        if (!name)
        {
            return FALSE;
        }

        g_strlcpy(ifname, name, ifname_len);
        return TRUE;
    }

    if (strcmp(scope->view_name, CLI_VIEW_IF_LOOP) == 0)
    {
        uint32_t loop_id = 0;
        if (cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_IF_LOOP_IDX, &loop_id) != 0 ||
            loop_id == 0)
        {
            return FALSE;
        }

        g_snprintf(ifname, ifname_len, "loop%u", loop_id);
        return TRUE;
    }

    return FALSE;
}

static gboolean isis_bdr_resolve_scoped_tag(const cli_show_scope_t *scope, uint32_t *tag_out)
{
    uint32_t tag = 0;

    if (!scope || !tag_out || strcmp(scope->view_name, CLI_VIEW_ISIS) != 0)
    {
        return FALSE;
    }

    if (cli_ctx_lookup_uint32(scope->ctx_data, scope->ctx_len, CLI_CTX_ID_ISIS_TAG, &tag) != 0 || tag == 0)
    {
        return FALSE;
    }

    *tag_out = tag;
    return TRUE;
}

static void append_instance_global_block(GString *out, db_row_t *row)
{
    uint32_t tag = (uint32_t)db_row_get_int(row, "tag", 0);
    if (tag == 0u)
    {
        return;
    }

    const char *net = db_row_get_text(row, "net", "");
    int64_t is_type = db_row_get_int(row, "is_type", ISIS_IS_TYPE_LEVEL_1_2);
    int64_t af4 = db_row_get_int(row, "af_ipv4", 1);
    int64_t af6 = db_row_get_int(row, "af_ipv6", 1);
    int64_t cost_style = db_row_get_int(row, "cost_style", ISIS_DEFAULT_COST_STYLE);
    const char *vrf_name = db_row_get_text(row, "vrf_name", "public");

    g_string_append(out, "!\r\n");
    g_string_append_printf(out, "isis %u", tag);
    if (vrf_name && vrf_name[0] != '\0' && strcmp(vrf_name, "public") != 0)
    {
        g_string_append_printf(out, " vrf %s", vrf_name);
    }
    g_string_append(out, "\r\n");
    if (net && net[0] != '\0')
    {
        g_string_append_printf(out, " net %s\r\n", net);
    }
    if ((uint8_t)is_type != ISIS_IS_TYPE_LEVEL_1_2)
    {
        g_string_append_printf(out, " is-type %s\r\n", is_type_to_text(is_type));
    }
    if ((uint8_t)cost_style != ISIS_DEFAULT_COST_STYLE && (uint8_t)cost_style == ISIS_COST_STYLE_WIDE)
    {
        g_string_append(out, " cost-style wide\r\n");
    }
    if (af4 == 0)
    {
        g_string_append(out, " no af ipv4\r\n");
    }
    if (af6 == 0)
    {
        g_string_append(out, " no af ipv6\r\n");
    }
    g_string_append(out, "!\r\n");
}

static void append_tag_if_entries(GString *out, dev_ipc_context_t *ctx, uint32_t tag, const char *ifname_filter)
{
    db_condition_t conds[2];
    guint num_conds = 0;

    conds[num_conds++] = (db_condition_t){
        .field_name = "tag",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)tag),
    };

    if (ifname_filter && ifname_filter[0] != '\0')
    {
        conds[num_conds++] = (db_condition_t){
            .field_name = "ifname",
            .op = DB_CMP_EQ,
            .value = db_value_text(ifname_filter),
        };
    }

    db_filter_t if_filter = {
        .conditions = conds,
        .num_conditions = num_conds,
    };

    db_result_t *if_result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INTERFACE, NULL, 0, &if_filter, &if_result) == ERRCODE_SUCCESS && if_result)
    {
        GHashTable *if_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        if (!if_map)
        {
            db_result_free(if_result);
            for (guint i = 0; i < num_conds; i++)
            {
                db_value_free(&conds[i].value);
            }
            return;
        }

        for (uint32_t j = 0; j < if_result->num_rows; j++)
        {
            db_row_t *if_row = if_result->rows[j];
            const char *ifname = db_row_get_text(if_row, "ifname", NULL);
            if (!ifname || ifname[0] == '\0')
            {
                continue;
            }

            isis_if_cfg_t *if_cfg = (isis_if_cfg_t *)g_hash_table_lookup(if_map, ifname);
            if (!if_cfg)
            {
                if_cfg = g_malloc0(sizeof(*if_cfg));
                if (!if_cfg)
                {
                    continue;
                }
                isis_bdr_if_cfg_defaults(if_cfg, ifname);
                g_hash_table_replace(if_map, g_strdup(ifname), if_cfg);
            }

            uint16_t afi = (uint16_t)db_row_get_int(if_row, "afi", 0);
            if (afi == ISIS_AFI_IPV4)
            {
                isis_bdr_set_af_cfg_from_row(&if_cfg->v4, if_row);
            }
            else if (afi == ISIS_AFI_IPV6)
            {
                isis_bdr_set_af_cfg_from_row(&if_cfg->v6, if_row);
            }
            else
            {
                isis_bdr_set_af_cfg_from_row(&if_cfg->v4, if_row);
                isis_bdr_set_af_cfg_from_row(&if_cfg->v6, if_row);
            }
        }

        GHashTableIter if_iter;
        gpointer if_key = NULL;
        gpointer if_val = NULL;
        g_hash_table_iter_init(&if_iter, if_map);
        while (g_hash_table_iter_next(&if_iter, &if_key, &if_val))
        {
            (void)if_key;
            append_if_entry(out, tag, (const isis_if_cfg_t *)if_val);
        }

        g_hash_table_destroy(if_map);
        db_result_free(if_result);
    }

    for (guint i = 0; i < num_conds; i++)
    {
        db_value_free(&conds[i].value);
    }
}

static int isis_bdr_show_config_full(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    GString *out = g_string_new("");
    if (!out)
    {
        isis_bdr_send_resp(msg, "");
        return ERRCODE_FAIL;
    }

    db_result_t *inst_result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, NULL, &inst_result) != ERRCODE_SUCCESS || !inst_result ||
        inst_result->num_rows == 0)
    {
        if (inst_result)
        {
            db_result_free(inst_result);
        }
        isis_bdr_send_resp(msg, out->str);
        g_string_free(out, TRUE);
        return ERRCODE_SUCCESS;
    }

    for (uint32_t i = 0; i < inst_result->num_rows; i++)
    {
        db_row_t *row = inst_result->rows[i];
        uint32_t tag = (uint32_t)db_row_get_int(row, "tag", 0);
        if (tag == 0u)
        {
            continue;
        }

        append_instance_global_block(out, row);
        append_tag_if_entries(out, ctx, tag, NULL);
    }

    db_result_free(inst_result);
    isis_bdr_send_resp(msg, out->str);
    g_string_free(out, TRUE);
    return ERRCODE_SUCCESS;
}

static int isis_bdr_show_config_scoped(dev_ipc_message_t *msg, const cli_show_scope_t *scope)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    GString *out = g_string_new("");
    if (!out)
    {
        isis_bdr_send_resp(msg, "");
        return ERRCODE_FAIL;
    }

    if (strcmp(scope->view_name, CLI_VIEW_ISIS) == 0)
    {
        uint32_t target_tag = 0;
        if (!isis_bdr_resolve_scoped_tag(scope, &target_tag))
        {
            isis_bdr_send_resp(msg, "");
            g_string_free(out, TRUE);
            return ERRCODE_SUCCESS;
        }

        db_condition_t cond = {.field_name = "tag", .op = DB_CMP_EQ, .value = db_value_int((int64_t)target_tag)};
        db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
        db_result_t *inst_result = NULL;
        if (db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, &filter, &inst_result) == ERRCODE_SUCCESS && inst_result &&
            inst_result->num_rows > 0)
        {
            append_instance_global_block(out, inst_result->rows[0]);
        }
        db_value_free(&cond.value);
        if (inst_result)
        {
            db_result_free(inst_result);
        }

        isis_bdr_send_resp(msg, out->str);
        g_string_free(out, TRUE);
        return ERRCODE_SUCCESS;
    }

    char ifname[32];
    if (!isis_bdr_resolve_scoped_ifname(scope, ifname, sizeof(ifname)))
    {
        isis_bdr_send_resp(msg, "");
        g_string_free(out, TRUE);
        return ERRCODE_SUCCESS;
    }

    db_result_t *inst_result = NULL;
    if (db_rpc_query(ctx, ISIS_TABLE_INSTANCE, NULL, 0, NULL, &inst_result) != ERRCODE_SUCCESS || !inst_result ||
        inst_result->num_rows == 0)
    {
        if (inst_result)
        {
            db_result_free(inst_result);
        }
        isis_bdr_send_resp(msg, out->str);
        g_string_free(out, TRUE);
        return ERRCODE_SUCCESS;
    }

    for (uint32_t i = 0; i < inst_result->num_rows; i++)
    {
        db_row_t *row = inst_result->rows[i];
        uint32_t tag = (uint32_t)db_row_get_int(row, "tag", 0);
        if (tag == 0u)
        {
            continue;
        }
        append_tag_if_entries(out, ctx, tag, ifname);
    }

    db_result_free(inst_result);
    isis_bdr_send_resp(msg, out->str);
    g_string_free(out, TRUE);
    return ERRCODE_SUCCESS;
}

int isis_bdr_handle_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        LOG_WARN("ISIS BDR: invalid SHOW_CONFIG scope payload");
        isis_bdr_send_resp(msg, "");
        return ERRCODE_FAIL;
    }

    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS)
    {
        return isis_bdr_show_config_scoped(msg, &scope);
    }

    return isis_bdr_show_config_full(msg);
}
