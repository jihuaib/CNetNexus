/**
 * @file   bgp_pkt_build_labeled.c
 * @brief  BGP UPDATE packet encoder for IPv4/IPv6 labeled-unicast (SAFI=4)
 */
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

#include "bgp_conn.h"
#include "bgp_pkt_build.h"

static int labeled_family_valid(uint16_t afi, const bgp_nexthop_t *nexthop)
{
    if (!nexthop || nexthop->global.family == 0)
    {
        return 0;
    }
    if (afi == BGP_AFI_IPV4)
    {
        return nexthop->global.family == AF_INET;
    }
    if (afi == BGP_AFI_IPV6)
    {
        return nexthop->global.family == AF_INET6;
    }
    return 0;
}

static int labeled_nlri_valid(const bgp_nlri_entry_t *nlri, uint16_t afi)
{
    if (!nlri || nlri->type != BGP_NLRI_PREFIX || nlri->afi != afi || nlri->safi != BGP_SAFI_LABELED ||
        !nlri->prefix.has_label || nlri->prefix.label > 0xFFFFFu)
    {
        return 0;
    }
    if (afi == BGP_AFI_IPV4)
    {
        return nlri->prefix.prefix.addr.family == AF_INET && nlri->prefix.prefix.prefix_len <= 32u;
    }
    if (afi == BGP_AFI_IPV6)
    {
        return nlri->prefix.prefix.addr.family == AF_INET6 && nlri->prefix.prefix.prefix_len <= 128u;
    }
    return 0;
}

static int encode_labeled_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
{
    if (!buf || !labeled_nlri_valid(nlri, nlri ? nlri->afi : 0))
    {
        return -1;
    }

    uint8_t plen = nlri->prefix.prefix.prefix_len;
    int nbytes = (plen + 7) / 8;
    if (buf_size < 1 + 3 + nbytes)
    {
        return -1;
    }

    uint32_t label = nlri->prefix.label & 0xFFFFFu;
    buf[0] = (uint8_t)(24u + plen);
    buf[1] = (uint8_t)(label >> 12);
    buf[2] = (uint8_t)(label >> 4);
    buf[3] = (uint8_t)(((label & 0xFu) << 4) | 0x01u);

    if (nlri->afi == BGP_AFI_IPV4)
    {
        memcpy(buf + 4, &nlri->prefix.prefix.addr.u.v4, nbytes);
    }
    else
    {
        memcpy(buf + 4, &nlri->prefix.prefix.addr.u.v6, nbytes);
    }
    return 1 + 3 + nbytes;
}

static int encode_mp_reach_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                  const bgp_nexthop_t *nexthop, uint16_t afi, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!buf || !nlri_list || nlri_count <= 0 || !labeled_family_valid(afi, nexthop))
    {
        return -1;
    }

    uint8_t nh_len = 0;
    if (afi == BGP_AFI_IPV4)
    {
        nh_len = 4;
    }
    else if (nexthop->has_link_local)
    {
        nh_len = 32;
    }
    else
    {
        nh_len = 16;
    }

    int header_len = 3 + 2 + 1 + 1 + (int)nh_len + 1;
    if (buf_size < header_len)
    {
        return -1;
    }

    int pos = 3;
    uint16_t afi_be = htons(afi);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_LABELED;
    buf[pos++] = nh_len;
    if (afi == BGP_AFI_IPV4)
    {
        memcpy(buf + pos, &nexthop->global.u.v4, 4);
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
        if (!labeled_nlri_valid(nlri, afi))
        {
            continue;
        }
        int n = encode_labeled_nlri(buf + pos, buf_size - pos, nlri);
        if (n < 0 || (pos - 3) + n > 255)
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

    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_REACH;
    buf[2] = (uint8_t)(pos - 3);
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

static int encode_mp_unreach_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                    int nlri_count, uint16_t afi, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!buf || !nlri_list || nlri_count <= 0)
    {
        return -1;
    }

    int header_len = 3 + 2 + 1;
    if (buf_size < header_len)
    {
        return -1;
    }

    int pos = 3;
    uint16_t afi_be = htons(afi);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_LABELED;

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!labeled_nlri_valid(nlri, afi))
        {
            continue;
        }
        int n = encode_labeled_nlri(buf + pos, buf_size - pos, nlri);
        if (n < 0 || (pos - 3) + n > 255)
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

    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_UNREACH;
    buf[2] = (uint8_t)(pos - 3);
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

static int labeled_encode_reach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                   const bgp_nexthop_t *nexthop)
{
    const bgp_nlri_entry_t *list[1] = {nlri};
    int packed = 0;
    return encode_mp_reach_packed(buf, buf_size, list, 1, nexthop, nlri ? nlri->afi : 0, &packed);
}

static int labeled_encode_reach_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                     const bgp_nexthop_t *nexthop)
{
    (void)buf;
    (void)buf_size;
    (void)nlri;
    (void)nexthop;
    return 0;
}

static int labeled_encode_unreach_wd(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)buf;
    (void)buf_size;
    (void)nlri;
    (void)conn;
    return 0;
}

static int labeled_encode_unreach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)conn;
    const bgp_nlri_entry_t *list[1] = {nlri};
    int packed = 0;
    return encode_mp_unreach_packed(buf, buf_size, list, 1, nlri ? nlri->afi : 0, &packed);
}

static int ipv4lu_encode_reach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                         int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    return encode_mp_reach_packed(buf, buf_size, nlri_list, nlri_count, nexthop, BGP_AFI_IPV4, out_packed);
}

static int ipv6lu_encode_reach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                         int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    return encode_mp_reach_packed(buf, buf_size, nlri_list, nlri_count, nexthop, BGP_AFI_IPV6, out_packed);
}

static int labeled_encode_reach_nlri_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
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

static int labeled_encode_unreach_wd_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
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

static int ipv4lu_encode_unreach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    (void)conn;
    return encode_mp_unreach_packed(buf, buf_size, nlri_list, nlri_count, BGP_AFI_IPV4, out_packed);
}

static int ipv6lu_encode_unreach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    (void)conn;
    return encode_mp_unreach_packed(buf, buf_size, nlri_list, nlri_count, BGP_AFI_IPV6, out_packed);
}

static const bgp_pkt_af_enc_t g_ipv4lu_enc = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_LABELED,
    .encode_reach_pa = labeled_encode_reach_pa,
    .encode_reach_nlri = labeled_encode_reach_nlri,
    .encode_unreach_wd = labeled_encode_unreach_wd,
    .encode_unreach_pa = labeled_encode_unreach_pa,
    .encode_reach_pa_packed = ipv4lu_encode_reach_pa_packed,
    .encode_reach_nlri_packed = labeled_encode_reach_nlri_packed,
    .encode_unreach_wd_packed = labeled_encode_unreach_wd_packed,
    .encode_unreach_pa_packed = ipv4lu_encode_unreach_pa_packed,
};

static const bgp_pkt_af_enc_t g_ipv6lu_enc = {
    .afi = BGP_AFI_IPV6,
    .safi = BGP_SAFI_LABELED,
    .encode_reach_pa = labeled_encode_reach_pa,
    .encode_reach_nlri = labeled_encode_reach_nlri,
    .encode_unreach_wd = labeled_encode_unreach_wd,
    .encode_unreach_pa = labeled_encode_unreach_pa,
    .encode_reach_pa_packed = ipv6lu_encode_reach_pa_packed,
    .encode_reach_nlri_packed = labeled_encode_reach_nlri_packed,
    .encode_unreach_wd_packed = labeled_encode_unreach_wd_packed,
    .encode_unreach_pa_packed = ipv6lu_encode_unreach_pa_packed,
};

void bgp_pkt_build_labeled_register(void)
{
    bgp_pkt_af_enc_register(&g_ipv4lu_enc);
    bgp_pkt_af_enc_register(&g_ipv6lu_enc);
}
