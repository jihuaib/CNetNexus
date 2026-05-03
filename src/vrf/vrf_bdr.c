/**
 * @file   vrf_bdr.c
 * @brief  VRF SHOW_CONFIG 实现：从 DB 生成 vrf 配置文本
 * @author jhb
 * @date   2026/05/02
 */
#include "vrf_bdr.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "vrf.h"
#include "vrf_db.h"
#include "vrf_main.h"

static void rd_to_str(const uint8_t *bytes, char *out, size_t cap)
{
    if (!bytes || cap < 8)
    {
        if (cap > 0)
        {
            out[0] = '\0';
        }
        return;
    }
    uint16_t type = ((uint16_t)bytes[0] << 8) | bytes[1];
    if (type == 0)
    {
        uint16_t asn = ((uint16_t)bytes[2] << 8) | bytes[3];
        uint32_t val = ((uint32_t)bytes[4] << 24) | ((uint32_t)bytes[5] << 16) | ((uint32_t)bytes[6] << 8) | bytes[7];
        snprintf(out, cap, "%u:%u", asn, val);
    }
    else if (type == 1)
    {
        uint16_t val = ((uint16_t)bytes[6] << 8) | bytes[7];
        snprintf(out, cap, "%u.%u.%u.%u:%u", bytes[2], bytes[3], bytes[4], bytes[5], val);
    }
    else
    {
        snprintf(out, cap, "raw:%02x%02x%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
                 bytes[5], bytes[6], bytes[7]);
    }
}

static int parse_hex16(const char *s, uint8_t out[8])
{
    if (!s || strlen(s) != 16)
    {
        return -1;
    }
    for (int i = 0; i < 8; i++)
    {
        unsigned int byte = 0;
        if (sscanf(s + i * 2, "%2x", &byte) != 1)
        {
            return -1;
        }
        out[i] = (uint8_t)byte;
    }
    return 0;
}

static const char *afi_kw(int64_t afi)
{
    return (afi == VRF_AFI_IPV4) ? "ipv4" : (afi == VRF_AFI_IPV6) ? "ipv6" : NULL;
}

static const char *safi_kw(int64_t safi)
{
    return (safi == VRF_SAFI_UNICAST) ? "unicast" : NULL;
}

static void send_resp(dev_ipc_message_t *msg, GString *buf)
{
    char *data = buf ? g_string_free(buf, FALSE) : g_strdup("");
    uint32_t len = (uint32_t)strlen(data) + 1;
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_VRF, msg->src_module_id,
                                                     msg->request_id, data, len, g_free);
    if (resp)
    {
        dev_ipc_send_response(vrf_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(data);
    }
}

static void emit_vrf_block(GString *out, dev_ipc_context_t *ctx, uint32_t vrf_id, const char *name)
{
    g_string_append(out, "!\r\n");
    g_string_append_printf(out, "vrf %s\r\n", name);

    /* AF + RD */
    db_filter_builder_t fb;
    db_filter_init(&fb);
    db_filter_add_int(&fb, "vrf_id", (int64_t)vrf_id);

    db_result_t *res = NULL;
    if (db_rpc_query(ctx, VRF_TABLE_AF, NULL, 0, &fb.filter, &res) == ERRCODE_SUCCESS && res)
    {
        for (uint32_t i = 0; i < res->num_rows; i++)
        {
            db_row_t *row = res->rows[i];
            int64_t afi = db_row_get_int(row, "afi", 0);
            int64_t safi = db_row_get_int(row, "safi", VRF_SAFI_UNICAST);
            const char *akw = afi_kw(afi);
            const char *skw = safi_kw(safi);
            if (!akw || !skw)
            {
                continue;
            }
            g_string_append_printf(out, " af %s-%s\r\n", akw, skw);
            int has_rd = (int)db_row_get_int(row, "has_rd", 0);
            const char *rd_hex = db_row_get_text(row, "rd", NULL);
            if (has_rd && rd_hex)
            {
                uint8_t rd_bytes[8];
                if (parse_hex16(rd_hex, rd_bytes) == 0)
                {
                    char rd_str[40];
                    rd_to_str(rd_bytes, rd_str, sizeof(rd_str));
                    g_string_append_printf(out, "  route-distinguisher %s\r\n", rd_str);
                }
            }

            /* 该 AF 下的 RT */
            db_filter_builder_t rfb;
            db_filter_init(&rfb);
            db_filter_add_int(&rfb, "vrf_id", (int64_t)vrf_id);
            db_filter_add_int(&rfb, "afi", afi);
            db_filter_add_int(&rfb, "safi", safi);

            db_result_t *rt_res = NULL;
            if (db_rpc_query(ctx, VRF_TABLE_RT, NULL, 0, &rfb.filter, &rt_res) == ERRCODE_SUCCESS && rt_res)
            {
                for (uint32_t k = 0; k < rt_res->num_rows; k++)
                {
                    db_row_t *r = rt_res->rows[k];
                    const char *hex = db_row_get_text(r, "rt", NULL);
                    int64_t dir = db_row_get_int(r, "direction", 0);
                    if (!hex)
                    {
                        continue;
                    }
                    uint8_t rt_bytes[8];
                    if (parse_hex16(hex, rt_bytes) != 0)
                    {
                        continue;
                    }
                    char rt_str[40];
                    rd_to_str(rt_bytes, rt_str, sizeof(rt_str));
                    const char *dir_kw = (dir == 0) ? "import" : "export";
                    g_string_append_printf(out, "  vpn-target %s %s\r\n", rt_str, dir_kw);
                }
                db_result_free(rt_res);
            }
            db_filter_clear(&rfb);
        }
        db_result_free(res);
    }
    db_filter_clear(&fb);
}

void vrf_bdr_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        send_resp(msg, NULL);
        return;
    }

    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    GString *out = g_string_new("");

    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS)
    {
        if (strcmp(scope.view_name, CLI_VIEW_VRF) != 0 && strcmp(scope.view_name, CLI_VIEW_VRF_AF_IPV4) != 0 &&
            strcmp(scope.view_name, CLI_VIEW_VRF_AF_IPV6) != 0)
        {
            send_resp(msg, out);
            return;
        }
        char vrf_name[VRF_NAME_MAX_LEN] = {0};
        if (cli_ctx_lookup_text(scope.ctx_data, scope.ctx_len, CLI_CTX_ID_VRF_NAME, vrf_name, sizeof(vrf_name)) != 0 ||
            vrf_name[0] == '\0')
        {
            send_resp(msg, out);
            return;
        }
        db_filter_builder_t fb;
        db_filter_init(&fb);
        db_filter_add_text(&fb, "name", vrf_name);
        db_result_t *res = NULL;
        if (db_rpc_query(ctx, VRF_TABLE_INSTANCE, NULL, 0, &fb.filter, &res) == ERRCODE_SUCCESS && res &&
            res->num_rows > 0)
        {
            uint32_t vrf_id = (uint32_t)db_row_get_int(res->rows[0], "vrf_id", 0);
            const char *name = db_row_get_text(res->rows[0], "name", vrf_name);
            emit_vrf_block(out, ctx, vrf_id, name);
            g_string_append(out, "!\r\n");
        }
        if (res)
        {
            db_result_free(res);
        }
        db_filter_clear(&fb);
        send_resp(msg, out);
        return;
    }

    /* FULL：枚举所有非公网 VRF */
    db_result_t *res = NULL;
    if (db_rpc_query(ctx, VRF_TABLE_INSTANCE, NULL, 0, NULL, &res) == ERRCODE_SUCCESS && res)
    {
        for (uint32_t i = 0; i < res->num_rows; i++)
        {
            db_row_t *row = res->rows[i];
            uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", 0);
            const char *name = db_row_get_text(row, "name", NULL);
            if (!name || vrf_id == VRF_PUBLIC_VRF_ID)
            {
                continue;
            }
            emit_vrf_block(out, ctx, vrf_id, name);
        }
        if (out->len > 0)
        {
            g_string_append(out, "!\r\n");
        }
        db_result_free(res);
    }
    send_resp(msg, out);
}
