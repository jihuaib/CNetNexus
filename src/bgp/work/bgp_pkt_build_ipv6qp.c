/**
 * @file   bgp_pkt_build_ipv6qp.c
 * @brief  BGP UPDATE 报文 IPv6 QP（AFI=2, SAFI=253）AF 编码器
 *         NLRI 变长 TLV：长度(1B) + TLV1[type=1,bit-len,dqpn(1-3B)] + TLV2[type=2,len=mask,prefix]
 * @author jhb
 * @date   2026/04/20
 */
#include <arpa/inet.h>
#include <stdbool.h>
#include <string.h>

#include "bgp_conn.h"
#include "bgp_pkt_build.h"

// ============================================================================
// QP TLV 编码辅助
// ============================================================================

static int dqpn_wire_bits(uint32_t dqpn)
{
    if (dqpn <= 0xFFu)
    {
        return 8;
    }
    if (dqpn <= 0xFFFFu)
    {
        return 16;
    }
    return 24;
}

static int dqpn_bytes_from_bits(int dqpn_bits)
{
    return (dqpn_bits + 7) / 8;
}

static bool dqpn_bits_valid(uint32_t dqpn, int dqpn_bits)
{
    return dqpn <= 0xFFFFFFu && dqpn_bits >= 1 && dqpn_bits <= 24 && (dqpn_bits == 24 || (dqpn >> dqpn_bits) == 0);
}

static int qp_nlri_size(const bgp_nlri_entry_t *nlri)
{
    if (!nlri || nlri->type != BGP_NLRI_QP)
    {
        return -1;
    }
    int dq_bits = nlri->qp.dqpn_len ? nlri->qp.dqpn_len : dqpn_wire_bits(nlri->qp.dqpn);
    if (!dqpn_bits_valid(nlri->qp.dqpn, dq_bits))
    {
        return -1;
    }
    int dq = dqpn_bytes_from_bits(dq_bits);
    int pfx = (nlri->qp.prefix.prefix_len + 7) / 8;
    /* 外层长度(1) + DQPN TLV + PREFIX TLV(type+len(mask bits)+prefix bytes) */
    return 1 + (1 + 1 + dq) + (1 + 1 + pfx);
}

static int encode_qp_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
{
    if (!nlri || nlri->type != BGP_NLRI_QP)
    {
        return -1;
    }
    int dq_bits = nlri->qp.dqpn_len ? nlri->qp.dqpn_len : dqpn_wire_bits(nlri->qp.dqpn);
    if (!dqpn_bits_valid(nlri->qp.dqpn, dq_bits))
    {
        return -1;
    }
    int dq = dqpn_bytes_from_bits(dq_bits);
    uint8_t plen = nlri->qp.prefix.prefix_len;
    if (plen > 128)
    {
        return -1;
    }
    int pfx_bytes = (plen + 7) / 8;
    int value_len = (1 + 1 + dq) + (1 + 1 + pfx_bytes);
    int total = 1 + value_len;
    if (buf_size < total)
    {
        return -1;
    }

    int pos = 0;
    buf[pos++] = (uint8_t)value_len;

    buf[pos++] = BGP_QP_TLV_DQPN;
    buf[pos++] = (uint8_t)dq_bits;
    for (int i = dq - 1; i >= 0; i--)
    {
        buf[pos++] = (uint8_t)((nlri->qp.dqpn >> (i * 8)) & 0xFFu);
    }

    buf[pos++] = BGP_QP_TLV_PREFIX;
    /* TLV len 语义为 masklen(bit)，value 仅携带 prefix bytes。 */
    buf[pos++] = plen;
    if (pfx_bytes > 0)
    {
        memcpy(buf + pos, &nlri->qp.prefix.addr.u.v6, pfx_bytes);
        pos += pfx_bytes;
    }
    return pos;
}

// ============================================================================
// 单条编码（非 packed）
// ============================================================================

static int encode_mp_reach(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_nexthop_t *nexthop)
{
    if (!nlri || !nexthop || nexthop->global.family == 0 || nlri->type != BGP_NLRI_QP)
    {
        return -1;
    }

    uint8_t nh_len = 0;
    if (nexthop->global.family == AF_INET)
    {
        nh_len = 4;
    }
    else if (nexthop->global.family == AF_INET6)
    {
        nh_len = nexthop->has_link_local ? 32 : 16;
    }
    else
    {
        return -1;
    }

    int nlri_size = qp_nlri_size(nlri);
    if (nlri_size < 0)
    {
        return -1;
    }
    int value_len = 2 + 1 + 1 + (int)nh_len + 1 + nlri_size;
    int attr_total = 3 + value_len;
    if (buf_size < attr_total)
    {
        return -1;
    }

    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_REACH;
    buf[2] = (uint8_t)value_len;
    int pos = 3;
    uint16_t afi_be = htons(nlri->afi);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = nlri->safi;
    buf[pos++] = nh_len;

    if (nexthop->global.family == AF_INET)
    {
        memcpy(buf + pos, &nexthop->global.u.v4.s_addr, 4);
        pos += 4;
    }
    else
    {
        memcpy(buf + pos, &nexthop->global.u.v6, 16);
        pos += 16;
        if (nexthop->has_link_local)
        {
            memcpy(buf + pos, &nexthop->link_local.u.v6, 16);
            pos += 16;
        }
    }

    buf[pos++] = 0; /* SNPA count = 0 */
    int n = encode_qp_nlri(buf + pos, buf_size - pos, nlri);
    if (n < 0)
    {
        return -1;
    }
    pos += n;
    return pos;
}

static int encode_mp_unreach(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
{
    if (!nlri || nlri->type != BGP_NLRI_QP)
    {
        return -1;
    }
    int nlri_size = qp_nlri_size(nlri);
    if (nlri_size < 0)
    {
        return -1;
    }
    int value_len = 2 + 1 + nlri_size;
    int attr_total = 3 + value_len;
    if (buf_size < attr_total)
    {
        return -1;
    }

    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_UNREACH;
    buf[2] = (uint8_t)value_len;
    int pos = 3;
    uint16_t afi_be = htons(nlri->afi);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = nlri->safi;
    int n = encode_qp_nlri(buf + pos, buf_size - pos, nlri);
    if (n < 0)
    {
        return -1;
    }
    pos += n;
    return pos;
}

static int ipv6qp_encode_reach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                  const bgp_nexthop_t *nexthop)
{
    return encode_mp_reach(buf, buf_size, nlri, nexthop);
}

static int ipv6qp_encode_reach_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                    const bgp_nexthop_t *nexthop)
{
    (void)buf;
    (void)buf_size;
    (void)nlri;
    (void)nexthop;
    return 0;
}

static int ipv6qp_encode_unreach_wd(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)buf;
    (void)buf_size;
    (void)nlri;
    (void)conn;
    return 0;
}

static int ipv6qp_encode_unreach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)conn;
    return encode_mp_unreach(buf, buf_size, nlri);
}

// ============================================================================
// Packed 版本
// ============================================================================

static int ipv6qp_encode_reach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                         int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!nexthop || nexthop->global.family == 0)
    {
        return -1;
    }

    uint8_t nh_len = 0;
    if (nexthop->global.family == AF_INET)
    {
        nh_len = 4;
    }
    else if (nexthop->global.family == AF_INET6)
    {
        nh_len = nexthop->has_link_local ? 32 : 16;
    }
    else
    {
        return -1;
    }

    int header_len = 3 + 2 + 1 + 1 + (int)nh_len + 1;
    if (buf_size < header_len)
    {
        return -1;
    }

    int pos = 3;
    uint16_t afi_be = htons(BGP_AFI_IPV6);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_QP;
    buf[pos++] = nh_len;
    if (nexthop->global.family == AF_INET)
    {
        memcpy(buf + pos, &nexthop->global.u.v4.s_addr, 4);
        pos += 4;
    }
    else
    {
        memcpy(buf + pos, &nexthop->global.u.v6, 16);
        pos += 16;
        if (nexthop->has_link_local)
        {
            memcpy(buf + pos, &nexthop->link_local.u.v6, 16);
            pos += 16;
        }
    }
    buf[pos++] = 0; /* SNPA count */

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!nlri || nlri->type != BGP_NLRI_QP)
        {
            continue;
        }
        int need = qp_nlri_size(nlri);
        if (need < 0 || (pos - 3) + need > 255)
        {
            break;
        }
        int n = encode_qp_nlri(buf + pos, buf_size - pos, nlri);
        if (n < 0)
        {
            break;
        }
        pos += n;
        packed++;
    }
    if (packed == 0)
    {
        return -1;
    }

    int value_len = pos - 3;
    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_REACH;
    buf[2] = (uint8_t)value_len;
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

static int ipv6qp_encode_reach_nlri_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    (void)buf;
    (void)buf_size;
    (void)nlri_list;
    (void)nexthop;
    if (out_packed)
    {
        *out_packed = nlri_count;
    }
    return 0;
}

static int ipv6qp_encode_unreach_wd_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    (void)buf;
    (void)buf_size;
    (void)nlri_list;
    (void)conn;
    if (out_packed)
    {
        *out_packed = nlri_count;
    }
    return 0;
}

static int ipv6qp_encode_unreach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    (void)conn;
    if (out_packed)
    {
        *out_packed = 0;
    }

    int header_len = 3 + 2 + 1;
    if (buf_size < header_len)
    {
        return -1;
    }
    int pos = 3;
    uint16_t afi_be = htons(BGP_AFI_IPV6);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_QP;

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!nlri || nlri->type != BGP_NLRI_QP)
        {
            continue;
        }
        int need = qp_nlri_size(nlri);
        if (need < 0 || (pos - 3) + need > 255)
        {
            break;
        }
        int n = encode_qp_nlri(buf + pos, buf_size - pos, nlri);
        if (n < 0)
        {
            break;
        }
        pos += n;
        packed++;
    }
    if (packed == 0)
    {
        return -1;
    }

    int value_len = pos - 3;
    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_UNREACH;
    buf[2] = (uint8_t)value_len;
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

static const bgp_pkt_af_enc_t g_ipv6qp_enc = {
    .afi = BGP_AFI_IPV6,
    .safi = BGP_SAFI_QP,
    .encode_reach_pa = ipv6qp_encode_reach_pa,
    .encode_reach_nlri = ipv6qp_encode_reach_nlri,
    .encode_unreach_wd = ipv6qp_encode_unreach_wd,
    .encode_unreach_pa = ipv6qp_encode_unreach_pa,
    .encode_reach_pa_packed = ipv6qp_encode_reach_pa_packed,
    .encode_reach_nlri_packed = ipv6qp_encode_reach_nlri_packed,
    .encode_unreach_wd_packed = ipv6qp_encode_unreach_wd_packed,
    .encode_unreach_pa_packed = ipv6qp_encode_unreach_pa_packed,
};

void bgp_pkt_build_ipv6qp_register(void)
{
    bgp_pkt_af_enc_register(&g_ipv6qp_enc);
}
