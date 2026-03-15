/**
 * @file   bgp_parse_evpn.c
 * @brief  AFI=25 SAFI=70（L2VPN EVPN）NLRI 处理器（RFC 7432）
 * @author jhb
 * @date   2026/03/11
 *
 * EVPN NLRI 由一段连续的 Route Type 条目组成：
 *   Route Type  (1B)
 *   Length      (1B) = 本条目后续字节数
 *   Value       (Length B)
 *
 * 支持的路由类型：
 *   1 = Ethernet A-D（自动发现）
 *   2 = MAC/IP Advertisement
 *   3 = Inclusive Multicast Ethernet Tag（IMET）
 *   4 = Ethernet Segment
 *   5 = IP Prefix Route
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_parse_priv.h"

/* ============================================================================
 * 辅助：从 1 字节 IP 长度 + 可变 IP 字节 读取地址
 * ========================================================================== */

static bool read_ip_field(const uint8_t *data, uint16_t avail, net_addr_t *addr, uint16_t *consumed)
{
    if (avail < 1)
    {
        return false;
    }

    uint8_t ip_len_bits = data[0]; /* 0, 32 或 128 */

    if (ip_len_bits == 0)
    {
        addr->family = 0; /* 无效 */
        *consumed = 1;
        return true;
    }

    uint8_t ip_bytes = ip_len_bits / 8;
    if (ip_len_bits != 32 && ip_len_bits != 128)
    {
        return false;
    }
    if (1 + ip_bytes > avail)
    {
        return false;
    }

    if (ip_len_bits == 32)
    {
        addr->family = AF_INET;
        memcpy(&addr->u.v4, data + 1, 4);
    }
    else
    {
        addr->family = AF_INET6;
        memcpy(addr->u.v6.s6_addr, data + 1, 16);
    }

    *consumed = (uint16_t)(1 + ip_bytes);
    return true;
}

/* ============================================================================
 * 辅助：net_addr_t 转字符串
 * ========================================================================== */

static void addr_to_str(const net_addr_t *addr, char *buf, size_t sz)
{
    if (addr->family == AF_INET)
    {
        inet_ntop(AF_INET, &addr->u.v4, buf, (socklen_t)sz);
    }
    else if (addr->family == AF_INET6)
    {
        inet_ntop(AF_INET6, &addr->u.v6, buf, (socklen_t)sz);
    }
    else
    {
        snprintf(buf, sz, "0.0.0.0");
    }
}

/* ============================================================================
 * Type 1：Ethernet A-D Route
 * RD(8) + ESI(10) + EthTag(4) + Label(3)  = 25 bytes
 * ========================================================================== */

static bool parse_evpn_type1(const uint8_t *val, uint8_t vlen, bgp_nlri_evpn_t *e)
{
    if (vlen < 25)
    {
        return false;
    }

    memcpy(e->rd.bytes, val, 8);
    memcpy(e->esi.bytes, val + 8, 10);
    e->eth_tag = ((uint32_t)val[18] << 24) | ((uint32_t)val[19] << 16) | ((uint32_t)val[20] << 8) | val[21];
    e->label1 = bgp_label_decode(val + 22);
    return true;
}

/* ============================================================================
 * Type 2：MAC/IP Advertisement
 * RD(8) + ESI(10) + EthTag(4) + MAC_len(1,=48) + MAC(6) + IP_len(1) + IP(0/4/16)
 * + Label1(3) + Label2(3, optional)
 * ========================================================================== */

static bool parse_evpn_type2(const uint8_t *val, uint8_t vlen, bgp_nlri_evpn_t *e)
{
    if (vlen < 24)
    {
        return false;
    }

    uint16_t pos = 0;
    memcpy(e->rd.bytes, val + pos, 8);
    pos += 8;
    memcpy(e->esi.bytes, val + pos, 10);
    pos += 10;
    e->eth_tag =
        ((uint32_t)val[pos] << 24) | ((uint32_t)val[pos + 1] << 16) | ((uint32_t)val[pos + 2] << 8) | val[pos + 3];
    pos += 4;

    /* MAC 地址长度（固定 48 bit = 6 字节） */
    if (pos >= vlen)
    {
        return false;
    }
    uint8_t mac_len = val[pos++];
    if (mac_len != 48 || pos + 6 > vlen)
    {
        return false;
    }
    memcpy(e->mac, val + pos, 6);
    e->has_mac = true;
    pos += 6;

    /* IP 地址（可选）*/
    if (pos < vlen)
    {
        net_addr_t ip = {0};
        uint16_t cons = 0;
        if (read_ip_field(val + pos, vlen - pos, &ip, &cons))
        {
            if (ip.family != 0)
            {
                e->ip = ip;
                e->has_ip = true;
            }
            pos += cons;
        }
    }

    /* Label1（3B）*/
    if (pos + 3 <= vlen)
    {
        e->label1 = bgp_label_decode(val + pos);
        pos += 3;
    }

    /* Label2（3B，可选）*/
    if (pos + 3 <= vlen)
    {
        e->label2 = bgp_label_decode(val + pos);
        e->has_label2 = true;
    }

    return true;
}

/* ============================================================================
 * Type 3：Inclusive Multicast Ethernet Tag（IMET）
 * RD(8) + EthTag(4) + IP_len(1) + IP(4/16)
 * ========================================================================== */

static bool parse_evpn_type3(const uint8_t *val, uint8_t vlen, bgp_nlri_evpn_t *e)
{
    if (vlen < 13)
    {
        return false;
    }

    memcpy(e->rd.bytes, val, 8);
    e->eth_tag = ((uint32_t)val[8] << 24) | ((uint32_t)val[9] << 16) | ((uint32_t)val[10] << 8) | val[11];

    net_addr_t ip = {0};
    uint16_t cons = 0;
    if (read_ip_field(val + 12, vlen - 12, &ip, &cons) && ip.family != 0)
    {
        e->ip = ip;
        e->has_ip = true;
    }

    return true;
}

/* ============================================================================
 * Type 4：Ethernet Segment Route
 * RD(8) + ESI(10) + IP_len(1) + IP(4/16)
 * ========================================================================== */

static bool parse_evpn_type4(const uint8_t *val, uint8_t vlen, bgp_nlri_evpn_t *e)
{
    if (vlen < 19)
    {
        return false;
    }

    memcpy(e->rd.bytes, val, 8);
    memcpy(e->esi.bytes, val + 8, 10);

    net_addr_t ip = {0};
    uint16_t cons = 0;
    if (read_ip_field(val + 18, vlen - 18, &ip, &cons) && ip.family != 0)
    {
        e->ip = ip;
        e->has_ip = true;
    }

    return true;
}

/* ============================================================================
 * Type 5：IP Prefix Route
 * RD(8) + ESI(10) + EthTag(4) + IPPrefixLen(1) + IPPrefix(4/16) + GW(4/16) + Label(3)
 * ========================================================================== */

static bool parse_evpn_type5(const uint8_t *val, uint8_t vlen, bgp_nlri_evpn_t *e)
{
    if (vlen < 23)
    {
        return false;
    }

    uint16_t pos = 0;
    memcpy(e->rd.bytes, val, 8);
    pos += 8;
    memcpy(e->esi.bytes, val + pos, 10);
    pos += 10;
    e->eth_tag =
        ((uint32_t)val[pos] << 24) | ((uint32_t)val[pos + 1] << 16) | ((uint32_t)val[pos + 2] << 8) | val[pos + 3];
    pos += 4;

    /* IP Prefix Length（bits）*/
    if (pos >= vlen)
    {
        return false;
    }
    uint8_t plen = val[pos++];

    /* 根据前缀长度判断 IPv4/IPv6 */
    int af;
    uint8_t ip_bytes;
    if (plen <= 32)
    {
        af = AF_INET;
        ip_bytes = 4;
    }
    else
    {
        af = AF_INET6;
        ip_bytes = 16;
    }

    if (pos + ip_bytes + ip_bytes + 3 > vlen)
    {
        return false;
    }

    /* IP Prefix */
    e->ip_prefix.addr.family = (sa_family_t)af;
    e->ip_prefix.prefix_len = plen;
    if (af == AF_INET)
    {
        memcpy(&e->ip_prefix.addr.u.v4, val + pos, 4);
    }
    else
    {
        memcpy(e->ip_prefix.addr.u.v6.s6_addr, val + pos, 16);
    }
    pos += ip_bytes;

    /* Gateway IP */
    e->gw_ip.family = (sa_family_t)af;
    if (af == AF_INET)
    {
        memcpy(&e->gw_ip.u.v4, val + pos, 4);
    }
    else
    {
        memcpy(e->gw_ip.u.v6.s6_addr, val + pos, 16);
    }
    pos += ip_bytes;

    /* Label */
    e->label1 = bgp_label_decode(val + pos);

    return true;
}

/* ============================================================================
 * EVPN entry_to_str（根据路由类型生成可读字符串）
 * ========================================================================== */

static void evpn_entry_to_str(const bgp_nlri_entry_t *entry, char *buf, size_t sz)
{
    if (!entry || !buf || sz == 0)
    {
        return;
    }

    const bgp_nlri_evpn_t *e = &entry->evpn;
    char rd_str[48];
    char esi_str[32];
    char ip_str[INET6_ADDRSTRLEN];
    bgp_rd_to_str(&e->rd, rd_str, sizeof(rd_str));

    switch (e->route_type)
    {
        case 1: /* A-D */
            snprintf(buf, sz, "evpn:1:rd=%s:ethag=%u:label=%u", rd_str, e->eth_tag, e->label1);
            break;

        case 2: /* MAC/IP */
        {
            char mac_str[20];
            snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", e->mac[0], e->mac[1], e->mac[2],
                     e->mac[3], e->mac[4], e->mac[5]);
            if (e->has_ip)
            {
                addr_to_str(&e->ip, ip_str, sizeof(ip_str));
                snprintf(buf, sz, "evpn:2:rd=%s:mac=%s:ip=%s", rd_str, mac_str, ip_str);
            }
            else
            {
                snprintf(buf, sz, "evpn:2:rd=%s:mac=%s", rd_str, mac_str);
            }
            break;
        }

        case 3: /* IMET */
            addr_to_str(&e->ip, ip_str, sizeof(ip_str));
            snprintf(buf, sz, "evpn:3:rd=%s:ethag=%u:ip=%s", rd_str, e->eth_tag, ip_str);
            break;

        case 4: /* Ethernet Segment */
            bgp_esi_to_str(&e->esi, esi_str, sizeof(esi_str));
            addr_to_str(&e->ip, ip_str, sizeof(ip_str));
            snprintf(buf, sz, "evpn:4:rd=%s:esi=%s:ip=%s", rd_str, esi_str, ip_str);
            break;

        case 5: /* IP Prefix */
        {
            char prefix_str[INET6_ADDRSTRLEN];
            addr_to_str(&e->ip_prefix.addr, prefix_str, sizeof(prefix_str));
            snprintf(buf, sz, "evpn:5:rd=%s:ethag=%u:%s/%u:label=%u", rd_str, e->eth_tag, prefix_str,
                     e->ip_prefix.prefix_len, e->label1);
            break;
        }

        default:
            snprintf(buf, sz, "evpn:type%u:rd=%s", e->route_type, rd_str);
            break;
    }
}

/* ============================================================================
 * 主 EVPN NLRI 解析
 * ========================================================================== */

static uint32_t count_evpn_routes(const uint8_t *data, uint16_t len)
{
    uint16_t pos = 0;
    uint32_t count = 0;

    while (pos + 2 <= len)
    {
        /* type(1) + length(1) + value(length) */
        uint8_t vlen = data[pos + 1];
        if (pos + 2 + vlen > len)
        {
            break;
        }
        pos += (uint16_t)(2 + vlen);
        count++;
    }

    return count;
}

static int parse_evpn_nlri(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    uint32_t count = count_evpn_routes(data, len);
    if (count == 0)
    {
        return 0;
    }

    *out = calloc(count, sizeof(bgp_nlri_entry_t));
    if (!*out)
    {
        return -1;
    }

    uint16_t pos = 0;
    uint32_t idx = 0;

    while (pos + 2 <= len && idx < count)
    {
        uint8_t route_type = data[pos];
        uint8_t vlen = data[pos + 1];

        if (pos + 2 + vlen > len)
        {
            break;
        }

        const uint8_t *val = data + pos + 2;
        bgp_nlri_entry_t *e = &(*out)[idx];

        e->afi = BGP_AFI_L2VPN;
        e->safi = BGP_SAFI_EVPN;
        e->type = BGP_NLRI_EVPN;
        e->evpn.route_type = route_type;

        /* 保存原始字节（含 type + length） */
        uint16_t raw_len = (uint16_t)(2 + vlen);
        if (raw_len <= sizeof(e->evpn.raw))
        {
            memcpy(e->evpn.raw, data + pos, raw_len);
            e->evpn.raw_len = raw_len;
        }

        bool ok;
        switch (route_type)
        {
            case 1:
                ok = parse_evpn_type1(val, vlen, &e->evpn);
                break;
            case 2:
                ok = parse_evpn_type2(val, vlen, &e->evpn);
                break;
            case 3:
                ok = parse_evpn_type3(val, vlen, &e->evpn);
                break;
            case 4:
                ok = parse_evpn_type4(val, vlen, &e->evpn);
                break;
            case 5:
                ok = parse_evpn_type5(val, vlen, &e->evpn);
                break;
            default:
                ok = true;
                break; /* 未知类型，保留原始字节 */
        }

        if (ok)
        {
            idx++;
        }

        pos += (uint16_t)(2 + vlen);
    }

    *out_len = idx;
    return 0;
}

/* ============================================================================
 * nexthop 解析（EVPN 支持 IPv4 4B 或 IPv6 16B）
 * ========================================================================== */

static int evpn_nexthop(const uint8_t *nh_data, uint8_t nh_len, bgp_nexthop_t *nexthop)
{
    if (nh_len == 4)
    {
        nexthop->global.family = AF_INET;
        memcpy(&nexthop->global.u.v4, nh_data, 4);
    }
    else if (nh_len >= 16)
    {
        nexthop->global.family = AF_INET6;
        memcpy(nexthop->global.u.v6.s6_addr, nh_data, 16);
        if (nh_len >= 32)
        {
            nexthop->link_local.family = AF_INET6;
            memcpy(nexthop->link_local.u.v6.s6_addr, nh_data + 16, 16);
            nexthop->has_link_local = true;
        }
    }
    else
    {
        return -1;
    }
    return 0;
}

/* ============================================================================
 * 处理器描述符与注册
 * ========================================================================== */

static const bgp_af_parser_t g_evpn = {
    .afi = BGP_AFI_L2VPN,
    .safi = BGP_SAFI_EVPN,
    .parse_reach = parse_evpn_nlri,
    .parse_unreach = parse_evpn_nlri,
    .parse_nexthop = evpn_nexthop,
    .entry_to_str = evpn_entry_to_str,
};

void bgp_parse_evpn_register(void)
{
    bgp_af_parser_register(&g_evpn);
}
