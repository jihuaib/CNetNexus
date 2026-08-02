/**
 * @file   bgp_pkt_build_ipv4uc.c
 * @brief  BGP UPDATE 报文 IPv4 单播（AFI=1, SAFI=1）AF 编码器
 *         支持传统 NEXT_HOP + NLRI（IPv4 nexthop）和
 *         MP_REACH_NLRI / MP_UNREACH_NLRI（IPv6 nexthop, RFC 8950）
 * @author jhb
 * @date   2026/03/15
 */
#include <arpa/inet.h>
#include <string.h>

#include "bgp_pkt_build.h"
#include "bgp_session.h"

// ============================================================================
// 辅助判断
// ============================================================================

/** 判断是否为双栈场景：IPv4 前缀但 nexthop 为 IPv6（RFC 8950） */
static gboolean is_ipv6_nexthop(const bgp_nexthop_t *nexthop)
{
    return nexthop && nexthop->global.family == AF_INET6;
}

/** 判断是否需要使用 MP_UNREACH（精确协商 IPv4-unicast/IPv6-NH tuple） */
static gboolean need_mp_unreach(const bgp_conn_t *conn)
{
    return conn && conn->session &&
           bgp_session_ext_nh_negotiated(conn->session, BGP_AFI_IPV4, BGP_SAFI_UNICAST, BGP_AFI_IPV6);
}

// ============================================================================
// IPv4 单播编码器实现
// ============================================================================

/**
 * @brief 编码传统 NEXT_HOP 路径属性（IPv4，4 字节）
 */
static int encode_nexthop_attr(uint8_t *buf, int buf_size, const net_addr_t *nh)
{
    if (nh->family != AF_INET)
    {
        return 0;
    }
    if (buf_size < 7)
    {
        return -1;
    }
    buf[0] = BGP_PA_FLAG_TRANSITIVE;
    buf[1] = BGP_PA_TYPE_NEXT_HOP;
    buf[2] = 4;
    memcpy(buf + 3, &nh->u.v4, 4);
    return 7;
}

/**
 * @brief 编码 MP_REACH_NLRI 路径属性（IPv4 前缀 + IPv6 nexthop, RFC 8950）
 *
 * 格式：AFI(2) + SAFI(1) + NHLen(1) + NH(16 or 32) + SNPA(1) + 前缀(1+n)
 */
static int encode_mp_reach_ipv4_v6nh(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                     const bgp_nexthop_t *nexthop)
{
    const net_prefix_t *pfx = &nlri->prefix.prefix;
    uint8_t plen = pfx->prefix_len;
    int pfx_bytes = 1 + (plen + 7) / 8;
    uint8_t nh_len = nexthop->has_link_local ? 32 : 16;
    /* 属性值长度：AFI(2)+SAFI(1)+NHLen(1)+NH(nh_len)+SNPA(1)+NLRI(pfx_bytes) */
    int value_len = 2 + 1 + 1 + (int)nh_len + 1 + pfx_bytes;
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
    memcpy(buf + pos, &nexthop->global.u.v6, 16);
    pos += 16;
    if (nexthop->has_link_local)
    {
        memcpy(buf + pos, &nexthop->link_local.u.v6, 16);
        pos += 16;
    }
    buf[pos++] = 0; /* SNPA count = 0 */
    /* IPv4 前缀 */
    buf[pos++] = plen;
    int nbytes = (plen + 7) / 8;
    memcpy(buf + pos, &pfx->addr.u.v4, nbytes);
    pos += nbytes;
    return pos;
}

/**
 * @brief 编码 MP_UNREACH_NLRI 路径属性（IPv4 前缀, RFC 8950）
 *
 * 格式：AFI(2) + SAFI(1) + 前缀(1+n)
 */
static int encode_mp_unreach_ipv4(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
{
    const net_prefix_t *pfx = &nlri->prefix.prefix;
    uint8_t plen = pfx->prefix_len;
    int pfx_bytes = 1 + (plen + 7) / 8;
    /* 属性值长度：AFI(2)+SAFI(1)+NLRI(pfx_bytes) */
    int value_len = 2 + 1 + pfx_bytes;
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
    buf[pos++] = plen;
    int nbytes = (plen + 7) / 8;
    memcpy(buf + pos, &pfx->addr.u.v4, nbytes);
    pos += nbytes;
    return pos;
}

/**
 * @brief 宣告 PA：IPv4 nexthop 用传统 NEXT_HOP，IPv6 nexthop 用 MP_REACH_NLRI
 */
static int ipv4uc_encode_reach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                  const bgp_nexthop_t *nexthop)
{
    if (is_ipv6_nexthop(nexthop))
    {
        return encode_mp_reach_ipv4_v6nh(buf, buf_size, nlri, nexthop);
    }
    return encode_nexthop_attr(buf, buf_size, &nexthop->global);
}

/**
 * @brief 宣告 NLRI：传统方式写入 NLRI 字段；双栈（IPv6 nexthop）时返回 0（已在 MP_REACH 中携带）
 */
static int ipv4uc_encode_reach_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                    const bgp_nexthop_t *nexthop)
{
    if (is_ipv6_nexthop(nexthop))
    {
        return 0;
    }
    return bgp_pkt_encode_prefix(buf, buf_size, &nlri->prefix.prefix);
}

/**
 * @brief 撤销 Withdrawn Routes：传统方式写入前缀；IPv6 peer 时返回 0（通过 MP_UNREACH 携带）
 */
static int ipv4uc_encode_unreach_wd(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    if (need_mp_unreach(conn))
    {
        return 0;
    }
    return bgp_pkt_encode_prefix(buf, buf_size, &nlri->prefix.prefix);
}

/**
 * @brief 撤销 PA：IPv6 peer（已协商 EXT_NH）使用 MP_UNREACH_NLRI（RFC 8950）；否则返回 0
 */
static int ipv4uc_encode_unreach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    if (need_mp_unreach(conn))
    {
        return encode_mp_unreach_ipv4(buf, buf_size, nlri);
    }
    return 0;
}

// ============================================================================
// Packed 版本（Phase 3：多 NLRI 共用属性）
// ============================================================================

/**
 * @brief Packed 宣告 PA：IPv4 nh 写 NEXT_HOP（不含 NLRI），IPv6 nh 写 MP_REACH 含多条前缀
 */
static int ipv4uc_encode_reach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                         int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!is_ipv6_nexthop(nexthop))
    {
        /* NEXT_HOP 固定 7 字节，NLRI 由 encode_reach_nlri_packed 处理 */
        int n = encode_nexthop_attr(buf, buf_size, &nexthop->global);
        if (n < 0)
        {
            return -1;
        }
        if (out_packed)
        {
            *out_packed = nlri_count;
        }
        return n;
    }

    /* RFC 8950：MP_REACH_NLRI 包含 AFI+SAFI+NHLen+NH+SNPA + 逐条 IPv4 前缀 */
    uint8_t nh_len = nexthop->has_link_local ? 32 : 16;
    int header_len = 3 + 2 + 1 + 1 + (int)nh_len + 1; /* attr hdr(3) + afi(2)+safi(1)+nhlen(1)+nh+snpa(1) */
    if (buf_size < header_len)
    {
        return -1;
    }
    /* 预留属性头部：flags + type + length(1B)。多条前缀时 length 可能 >= 255，
     * 但 IPv4 单条前缀最多 5 字节，255 长度可容纳约 49 条；为简化此处使用 1B length。*/
    int pos = 3;
    uint16_t afi_be = htons(BGP_AFI_IPV4);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_UNICAST;
    buf[pos++] = nh_len;
    memcpy(buf + pos, &nexthop->global.u.v6, 16);
    pos += 16;
    if (nexthop->has_link_local)
    {
        memcpy(buf + pos, &nexthop->link_local.u.v6, 16);
        pos += 16;
    }
    buf[pos++] = 0; /* SNPA count */

    /* 逐条追加 IPv4 前缀，直到空间不足或 PA length 超过 255 */
    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!nlri || nlri->type != BGP_NLRI_PREFIX)
        {
            continue;
        }
        /* 检查：属性值不得超过 255 字节（使用 1B length 字段） */
        uint8_t plen = nlri->prefix.prefix.prefix_len;
        int pfx_len = 1 + (plen + 7) / 8;
        if ((pos - 3) + pfx_len > 255)
        {
            break;
        }
        int n = bgp_pkt_encode_prefix(buf + pos, buf_size - pos, &nlri->prefix.prefix);
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

    /* 回填属性头部 */
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

/**
 * @brief Packed NLRI：IPv4 nh 时追加多条前缀；IPv6 nh 时前缀已在 MP_REACH 中
 */
static int ipv4uc_encode_reach_nlri_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (is_ipv6_nexthop(nexthop))
    {
        if (out_packed)
        {
            *out_packed = nlri_count;
        }
        return 0;
    }

    int pos = 0;
    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!nlri || nlri->type != BGP_NLRI_PREFIX)
        {
            continue;
        }
        int n = bgp_pkt_encode_prefix(buf + pos, buf_size - pos, &nlri->prefix.prefix);
        if (n < 0)
        {
            break;
        }
        pos += n;
        packed++;
    }
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

/**
 * @brief Packed 撤销 Withdrawn：IPv4 peer 追加多条前缀；IPv6 peer 返回 0
 */
static int ipv4uc_encode_unreach_wd_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (need_mp_unreach(conn))
    {
        if (out_packed)
        {
            *out_packed = nlri_count;
        }
        return 0;
    }

    int pos = 0;
    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!nlri || nlri->type != BGP_NLRI_PREFIX)
        {
            continue;
        }
        int n = bgp_pkt_encode_prefix(buf + pos, buf_size - pos, &nlri->prefix.prefix);
        if (n < 0)
        {
            break;
        }
        pos += n;
        packed++;
    }
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

/**
 * @brief Packed 撤销 PA：IPv6 peer 用 MP_UNREACH 嵌入多条 IPv4 前缀；IPv4 peer 返回 0
 */
static int ipv4uc_encode_unreach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!need_mp_unreach(conn))
    {
        if (out_packed)
        {
            *out_packed = nlri_count;
        }
        return 0;
    }

    /* MP_UNREACH 值：AFI(2) + SAFI(1) + 逐条前缀 */
    int header_len = 3 + 2 + 1; /* attr hdr + afi + safi */
    if (buf_size < header_len)
    {
        return -1;
    }
    int pos = 3;
    uint16_t afi_be = htons(BGP_AFI_IPV4);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_UNICAST;

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!nlri || nlri->type != BGP_NLRI_PREFIX)
        {
            continue;
        }
        uint8_t plen = nlri->prefix.prefix.prefix_len;
        int pfx_len = 1 + (plen + 7) / 8;
        if ((pos - 3) + pfx_len > 255)
        {
            break;
        }
        int n = bgp_pkt_encode_prefix(buf + pos, buf_size - pos, &nlri->prefix.prefix);
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

/** IPv4 单播编码器描述符 */
static const bgp_pkt_af_enc_t g_ipv4uc_enc = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_UNICAST,
    .encode_reach_pa = ipv4uc_encode_reach_pa,
    .encode_reach_nlri = ipv4uc_encode_reach_nlri,
    .encode_unreach_wd = ipv4uc_encode_unreach_wd,
    .encode_unreach_pa = ipv4uc_encode_unreach_pa,
    .encode_reach_pa_packed = ipv4uc_encode_reach_pa_packed,
    .encode_reach_nlri_packed = ipv4uc_encode_reach_nlri_packed,
    .encode_unreach_wd_packed = ipv4uc_encode_unreach_wd_packed,
    .encode_unreach_pa_packed = ipv4uc_encode_unreach_pa_packed,
};

void bgp_pkt_build_ipv4uc_register(void)
{
    bgp_pkt_af_enc_register(&g_ipv4uc_enc);
}
