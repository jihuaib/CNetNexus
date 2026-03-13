/**
 * @file   sbmp_bmp.c
 * @brief  BMP 报文解析辅助实现
 * @author jhb
 * @date   2026/03/13
 */
#include "sbmp_bmp.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/** per-peer flags: V=1 表示 IPv6 peer address */
#define SBMP_BMP_PEER_FLAG_IPV6 0x80
/** per-peer flags: L=1 表示 post-policy（L=0 表示 pre-policy） */
#define SBMP_BMP_PEER_FLAG_POST_POLICY 0x40
/** Loc-RIB flags: F=1 表示 Loc-RIB 经过过滤 */
#define SBMP_BMP_PEER_FLAG_LOC_RIB_FILTERED 0x80

static void bytes_to_hex(const uint8_t *in, uint32_t len, char *out, uint32_t out_len)
{
    static const char *hex = "0123456789abcdef";
    uint32_t off = 0;

    if (!in || !out || out_len == 0)
    {
        return;
    }

    for (uint32_t i = 0; i < len && (off + 2) < out_len; i++)
    {
        out[off++] = hex[(in[i] >> 4) & 0x0F];
        out[off++] = hex[in[i] & 0x0F];
    }
    out[off] = '\0';
}

int sbmp_bmp_parse_common_header(const uint8_t *msg, uint32_t msg_len, sbmp_bmp_common_view_t *out)
{
    uint32_t len_be = 0;

    if (!msg || msg_len < SBMP_BMP_COMMON_HDR_LEN || !out)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->version = msg[0];
    memcpy(&len_be, msg + 1, sizeof(len_be));
    out->msg_len = ntohl(len_be);
    out->msg_type = msg[5];

    if (out->version != 3)
    {
        return -1;
    }
    if (out->msg_len < SBMP_BMP_COMMON_HDR_LEN || out->msg_len > SBMP_BMP_MSG_LEN_MAX)
    {
        return -1;
    }
    if (msg_len < out->msg_len)
    {
        return -1;
    }

    return 0;
}

int sbmp_bmp_parse_peer_header(const uint8_t *peer_hdr, uint32_t hdr_len, sbmp_bmp_peer_view_t *out)
{
    uint8_t flags = 0;
    uint32_t as_be = 0;
    char rd_hex[17];

    if (!peer_hdr || hdr_len < SBMP_BMP_PEER_HDR_LEN || !out)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    out->peer_type = peer_hdr[0];
    flags = peer_hdr[1];
    out->is_loc_rib = (out->peer_type == SBMP_BMP_PEER_TYPE_LOC_RIB) ? 1 : 0;

    memcpy(&as_be, peer_hdr + 26, sizeof(as_be));
    out->peer_as = ntohl(as_be);

    if (!inet_ntop(AF_INET, peer_hdr + 30, out->bgp_id, sizeof(out->bgp_id)))
    {
        snprintf(out->bgp_id, sizeof(out->bgp_id), "0.0.0.0");
    }

    if (out->is_loc_rib)
    {
        out->is_loc_rib_filtered = (flags & SBMP_BMP_PEER_FLAG_LOC_RIB_FILTERED) ? 1 : 0;
        out->is_ipv6 = 0;
        out->is_post_policy = 0;

        bytes_to_hex(peer_hdr + 2, 8, rd_hex, sizeof(rd_hex));
        snprintf(out->peer_ip, sizeof(out->peer_ip), "loc-rib:%s:%s", out->bgp_id, rd_hex);
    }
    else
    {
        out->is_ipv6 = (flags & SBMP_BMP_PEER_FLAG_IPV6) ? 1 : 0;
        out->is_post_policy = (flags & SBMP_BMP_PEER_FLAG_POST_POLICY) ? 1 : 0;
        out->is_loc_rib_filtered = 0;

        if (out->is_ipv6)
        {
            if (!inet_ntop(AF_INET6, peer_hdr + 10, out->peer_ip, sizeof(out->peer_ip)))
            {
                return -1;
            }
        }
        else
        {
            if (!inet_ntop(AF_INET, peer_hdr + 22, out->peer_ip, sizeof(out->peer_ip)))
            {
                return -1;
            }
        }
    }

    return 0;
}

int sbmp_bmp_extract_route_monitoring(const uint8_t *msg, uint32_t msg_len, sbmp_bmp_peer_view_t *peer_out,
                                      const uint8_t **bgp_pdu, uint32_t *bgp_pdu_len)
{
    sbmp_bmp_common_view_t common;
    const uint8_t *bgp_body = NULL;
    uint32_t body_len = 0;

    if (!msg || !peer_out || !bgp_pdu || !bgp_pdu_len)
    {
        return -1;
    }

    if (sbmp_bmp_parse_common_header(msg, msg_len, &common) != 0)
    {
        return -1;
    }
    if (common.msg_type != SBMP_BMP_MSG_ROUTE_MONITORING)
    {
        return -1;
    }
    if (common.msg_len < SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN)
    {
        return -1;
    }
    if (sbmp_bmp_parse_peer_header(msg + SBMP_BMP_COMMON_HDR_LEN, SBMP_BMP_PEER_HDR_LEN, peer_out) != 0)
    {
        return -1;
    }

    bgp_body = msg + SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN;
    body_len = common.msg_len - SBMP_BMP_COMMON_HDR_LEN - SBMP_BMP_PEER_HDR_LEN;

    *bgp_pdu = bgp_body;
    *bgp_pdu_len = body_len;
    return 0;
}
