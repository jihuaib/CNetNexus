/**
 * @file   bgp_parse_ipv4qp.c
 * @brief  AFI=1 SAFI=253（IPv4 QP）NLRI 处理器
 *         变长 TLV 格式：长度(1B) + TLV1[type=1,bit-len,dqpn] + TLV2[type=2,len=mask,prefix]
 * @author jhb
 * @date   2026/04/20
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_parse_priv.h"

/**
 * @brief 解析单条 QP NLRI TLV 结构
 * @return 消耗字节数，-1=格式错误
 */
static int parse_qp_entry(const uint8_t *data, uint16_t avail, int af, bgp_nlri_entry_t *e)
{
    if (avail < 1)
    {
        return -1;
    }
    uint8_t value_len = data[0];
    if (1 + (uint16_t)value_len > avail)
    {
        return -1;
    }

    uint16_t pos = 1;
    uint16_t end = (uint16_t)(1 + value_len);
    bool have_dqpn = false;
    bool have_prefix = false;

    e->type = BGP_NLRI_QP;
    e->qp.dqpn = 0;
    e->qp.dqpn_len = 0;
    memset(&e->qp.prefix, 0, sizeof(e->qp.prefix));

    while (pos < end)
    {
        if (pos + 2 > end)
        {
            return -1;
        }
        uint8_t tlv_type = data[pos++];
        uint8_t tlv_len = data[pos++];

        if (tlv_type == BGP_QP_TLV_DQPN)
        {
            uint8_t dqpn_bits = tlv_len;
            uint8_t dqpn_bytes = (uint8_t)((dqpn_bits + 7u) / 8u);
            if (dqpn_bits < 1 || dqpn_bits > 24 || pos + dqpn_bytes > end)
            {
                return -1;
            }
            uint32_t v = 0;
            for (uint8_t i = 0; i < dqpn_bytes; i++)
            {
                v = (v << 8) | data[pos + i];
            }
            if (dqpn_bits < 24 && (v >> dqpn_bits) != 0)
            {
                return -1;
            }
            e->qp.dqpn = v;
            e->qp.dqpn_len = dqpn_bits;
            pos += dqpn_bytes;
            have_dqpn = true;
        }
        else if (tlv_type == BGP_QP_TLV_PREFIX)
        {
            uint8_t plen = tlv_len;
            uint8_t nbytes = (uint8_t)((plen + 7) / 8);
            if (plen > 32 || pos + nbytes > end)
            {
                return -1;
            }
            e->qp.prefix.prefix_len = plen;
            e->qp.prefix.addr.family = AF_INET;
            if (nbytes > 0)
            {
                memcpy(&e->qp.prefix.addr.u.v4, data + pos, nbytes);
            }
            pos += nbytes;
            have_prefix = true;
        }
        else
        {
            /* 未知 TLV，跳过 */
            if (pos + tlv_len > end)
            {
                return -1;
            }
            pos += tlv_len;
        }
    }

    (void)af;
    if (!have_dqpn || !have_prefix)
    {
        return -1;
    }
    return (int)end;
}

static uint32_t count_qp_entries(const uint8_t *data, uint16_t len)
{
    uint16_t pos = 0;
    uint32_t count = 0;
    while (pos < len)
    {
        uint8_t value_len = data[pos];
        if (1 + (uint16_t)value_len > len - pos)
        {
            break;
        }
        pos += (uint16_t)(1 + value_len);
        count++;
    }
    return count;
}

static int parse_qp_entries(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    uint32_t cap = count_qp_entries(data, len);
    if (cap == 0)
    {
        return 0;
    }
    *out = calloc(cap, sizeof(bgp_nlri_entry_t));
    if (!*out)
    {
        return -1;
    }

    uint16_t pos = 0;
    uint32_t idx = 0;
    while (pos < len && idx < cap)
    {
        bgp_nlri_entry_t *e = &(*out)[idx];
        int n = parse_qp_entry(data + pos, (uint16_t)(len - pos), AF_INET, e);
        if (n <= 0)
        {
            break;
        }
        e->afi = BGP_AFI_IPV4;
        e->safi = BGP_SAFI_QP;
        pos += (uint16_t)n;
        idx++;
    }
    *out_len = idx;
    return 0;
}

/* QP nexthop 与 IPv4 单播等价：4B 传统 / 16B or 32B IPv6（RFC 8950） */
static int parse_nexthop(const uint8_t *nh_data, uint8_t nh_len, bgp_nexthop_t *nexthop)
{
    if (nh_len == 4)
    {
        nexthop->global.family = AF_INET;
        memcpy(&nexthop->global.u.v4, nh_data, 4);
        nexthop->has_link_local = false;
        return 0;
    }
    if (nh_len == 16)
    {
        nexthop->global.family = AF_INET6;
        memcpy(&nexthop->global.u.v6, nh_data, 16);
        nexthop->has_link_local = false;
        return 0;
    }
    if (nh_len == 32)
    {
        nexthop->global.family = AF_INET6;
        memcpy(&nexthop->global.u.v6, nh_data, 16);
        nexthop->has_link_local = true;
        nexthop->link_local.family = AF_INET6;
        memcpy(&nexthop->link_local.u.v6, nh_data + 16, 16);
        return 0;
    }
    return -1;
}

static void entry_to_str(const bgp_nlri_entry_t *entry, char *buf, size_t sz)
{
    if (!entry || !buf || sz == 0)
    {
        return;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &entry->qp.prefix.addr.u.v4, ip, sizeof(ip));
    snprintf(buf, sz, "dqpn=%u,ip=%s/%u", entry->qp.dqpn, ip, entry->qp.prefix.prefix_len);
}

static const bgp_af_parser_t g_ipv4qp = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_QP,
    .parse_reach = parse_qp_entries,
    .parse_unreach = parse_qp_entries,
    .parse_nexthop = parse_nexthop,
    .entry_to_str = entry_to_str,
};

void bgp_parse_ipv4qp_register(void)
{
    bgp_af_parser_register(&g_ipv4qp);
}
