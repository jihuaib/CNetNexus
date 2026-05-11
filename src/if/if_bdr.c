/**
 * @file   if_bdr.c
 * @brief  IF 配置构建器：读取 DB 并生成 show current-configuration 输出
 *
 *         IF 模块是"接口"类 anchor 的属主: 对每个接口声明
 *         header "!\r\nif <name>\r\n" 和 footer "!\r\n", 自身的 IP/shutdown
 *         配置作为 body 追加; 其它模块(ISIS/OSPF/...) 可用相同 key
 *         "iface/<ifname>" 继续追加接口视图下的配置行, 由 CLI 聚合器去重合并。
 * @author jhb
 * @date   2026/03/08
 */
#include "if_bdr.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "if_main.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 内部辅助
// ============================================================================

static gboolean bdr_addr_is_valid(const char *ip_str, int64_t prefix_len, sa_family_t family)
{
    if (!ip_str || ip_str[0] == '\0')
    {
        return FALSE;
    }

    if ((family == AF_INET && (prefix_len < 0 || prefix_len > 32)) ||
        (family == AF_INET6 && (prefix_len < 0 || prefix_len > 128)))
    {
        return FALSE;
    }

    net_addr_t addr;
    if (net_addr_from_str(ip_str, &addr) != 0 || addr.family != family)
    {
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief 构造某接口的 anchor key 字符串: "iface/<ifname>"
 *        这是跨模块约定的公共契约: IF/ISIS/OSPF 均用此 key 访问同一接口容器。
 */
static void bdr_build_iface_key(char *buf, size_t buflen, const char *name)
{
    snprintf(buf, buflen, "iface/%s", name ? name : "");
}

/**
 * @brief 构造进入接口视图的命令行(含前导 "!\r\n" 分隔)
 *        loop 接口使用 "if loop <N>" 形式, 其余使用 "if <name>"。
 */
static void bdr_build_iface_header(char *buf, size_t buflen, const char *name)
{
    gboolean is_loop = (name && strncmp(name, "loop", 4) == 0 && name[4] >= '1' && name[4] <= '9');
    if (is_loop)
    {
        snprintf(buf, buflen, "!\r\nif loop %s\r\n", name + 4);
    }
    else
    {
        snprintf(buf, buflen, "!\r\nif %s\r\n", name ? name : "");
    }
}

static const char *bdr_if_ctx_idx_to_name(uint32_t if_idx)
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

static gboolean bdr_resolve_scoped_ifname(const cli_show_scope_t *scope, char *ifname, size_t ifname_len)
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

        const char *name = bdr_if_ctx_idx_to_name(if_idx);
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

static void bdr_emit_iface_anchor(GString *out, const char *name)
{
    char key[CLI_CFG_ANCHOR_KEY_MAX];
    char header[96];
    bdr_build_iface_key(key, sizeof(key), name);
    bdr_build_iface_header(header, sizeof(header), name);
    cli_cfg_anchor_emit_header(out, key, header);
    cli_cfg_anchor_emit_footer(out, key, "!\r\n");
}

/**
 * @brief 将 IF 自身对某接口的配置(IP/shutdown)发射为 body 贡献
 */
static void bdr_emit_iface_self_body(GString *out, const char *name, const char *ip4_str, int64_t prefix4_len,
                                     const char *ip6_str, int64_t prefix6_len, int64_t shutdown, const char *vrf_name)
{
    char key[CLI_CFG_ANCHOR_KEY_MAX];
    bdr_build_iface_key(key, sizeof(key), name);

    gboolean has_ip4 = bdr_addr_is_valid(ip4_str, prefix4_len, AF_INET);
    gboolean has_ip6 = bdr_addr_is_valid(ip6_str, prefix6_len, AF_INET6);
    gboolean is_shutdown = (shutdown != 0);
    gboolean has_vrf = (vrf_name && vrf_name[0] != '\0');

    if (!has_vrf && !has_ip4 && !has_ip6 && !is_shutdown)
    {
        return;
    }

    GString *body = g_string_new("");
    if (has_vrf)
    {
        g_string_append_printf(body, " vrf forwarding %s\r\n", vrf_name);
    }
    if (has_ip4)
    {
        g_string_append_printf(body, " ip address %s %ld\r\n", ip4_str, prefix4_len);
    }
    if (has_ip6)
    {
        g_string_append_printf(body, " ipv6 address %s %ld\r\n", ip6_str, prefix6_len);
    }
    if (is_shutdown)
    {
        g_string_append(body, " shutdown\r\n");
    }

    cli_cfg_anchor_emit_body(out, key, body->str);
    g_string_free(body, TRUE);
}

// ============================================================================
// 公共 API
// ============================================================================

static void bdr_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    const char *safe = text ? text : "";
    char *resp_data = g_strdup(safe);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_IF, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(if_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(resp_data);
    }
}

static void if_bdr_show_config_full(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    GString *out = g_string_new("");
    if (!out)
    {
        bdr_send_cli_response(msg, "");
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, "if_interface", NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        bdr_send_cli_response(msg, out->str);
        g_string_free(out, TRUE);
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *name = db_row_get_text(row, "name", NULL);
        const char *ip4_str = db_row_get_text(row, "ip_address", NULL);
        int64_t prefix4_len = db_row_get_int(row, "prefix_len", 0);
        const char *ip6_str = db_row_get_text(row, "ipv6_address", NULL);
        int64_t prefix6_len = db_row_get_int(row, "ipv6_prefix_len", 0);
        int64_t shutdown = db_row_get_int(row, "shutdown", 0);
        const char *vrf_name = db_row_get_text(row, "vrf_name", "");

        if (!name)
        {
            continue;
        }

        bdr_emit_iface_anchor(out, name);
        bdr_emit_iface_self_body(out, name, ip4_str, prefix4_len, ip6_str, prefix6_len, shutdown, vrf_name);
    }

    db_result_free(result);
    bdr_send_cli_response(msg, out->str);
    g_string_free(out, TRUE);
}

static void if_bdr_show_config_scoped(dev_ipc_message_t *msg, const cli_show_scope_t *scope)
{
    char ifname[32];
    if (!bdr_resolve_scoped_ifname(scope, ifname, sizeof(ifname)))
    {
        bdr_send_cli_response(msg, "");
        return;
    }

    GString *out = g_string_new("");
    if (!out)
    {
        bdr_send_cli_response(msg, "");
        return;
    }

    /*
     * scoped show this 下，IF 作为接口 anchor owner 需要始终提供 header/footer，
     * 即使本模块当前没有 IP/shutdown 配置，也要给其它模块的 body 贡献提供容器。
     */
    bdr_emit_iface_anchor(out, ifname);

    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_result_t *result = NULL;

    if (db_rpc_query(ctx, "if_interface", NULL, 0, &filter, &result) == ERRCODE_SUCCESS && result &&
        result->num_rows > 0)
    {
        db_row_t *row = result->rows[0];
        const char *ip4_str = db_row_get_text(row, "ip_address", NULL);
        int64_t prefix4_len = db_row_get_int(row, "prefix_len", 0);
        const char *ip6_str = db_row_get_text(row, "ipv6_address", NULL);
        int64_t prefix6_len = db_row_get_int(row, "ipv6_prefix_len", 0);
        int64_t shutdown = db_row_get_int(row, "shutdown", 0);
        const char *vrf_name = db_row_get_text(row, "vrf_name", "");

        bdr_emit_iface_self_body(out, ifname, ip4_str, prefix4_len, ip6_str, prefix6_len, shutdown, vrf_name);
    }

    db_value_free(&cond.value);
    if (result)
    {
        db_result_free(result);
    }

    bdr_send_cli_response(msg, out->str);
    g_string_free(out, TRUE);
}

void if_bdr_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        LOG_WARN("IF BDR: invalid SHOW_CONFIG scope payload");
        bdr_send_cli_response(msg, "");
        return;
    }

    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS)
    {
        if_bdr_show_config_scoped(msg, &scope);
        return;
    }

    if_bdr_show_config_full(msg);
}
