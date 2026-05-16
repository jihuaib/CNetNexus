/**
 * @file   bgp_parse_attr.c
 * @brief  BGP UPDATE 路径属性解析
 * @author jhb
 * @date   2026/03/11
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "bgp_parse_priv.h"

/* ============================================================================
 * 路径属性类型码
 * ========================================================================== */

#define ATTR_ORIGIN 1
#define ATTR_AS_PATH 2
#define ATTR_NEXT_HOP 3
#define ATTR_MULTI_EXIT_DISC 4
#define ATTR_LOCAL_PREF 5
#define ATTR_ATOMIC_AGGREGATE 6
#define ATTR_AGGREGATOR 7
#define ATTR_COMMUNITY 8
#define ATTR_ORIGINATOR_ID 9
#define ATTR_CLUSTER_LIST 10
#define ATTR_MP_REACH_NLRI 14
#define ATTR_MP_UNREACH_NLRI 15
#define ATTR_EXT_COMMUNITIES 16
#define ATTR_AS4_PATH 17
#define ATTR_AS4_AGGREGATOR 18
#define ATTR_LARGE_COMMUNITY 32

/* ============================================================================
 * AS_PATH 解析
 * ========================================================================== */

static void parse_as_path(const uint8_t *data, uint16_t len, bool as4, char *buf, size_t bufsz)
{
    size_t pos = 0;
    size_t wpos = 0;
    bool first_seg = true;

    while (pos + 2 <= len)
    {
        uint8_t seg_type = data[pos++];
        uint8_t seg_len = data[pos++]; /* ASN 数量，非字节数 */
        uint8_t asn_size = as4 ? 4 : 2;

        if (pos + (size_t)seg_len * asn_size > len)
        {
            break;
        }

        if (!first_seg && wpos < bufsz - 1)
        {
            buf[wpos++] = ' ';
        }
        first_seg = false;

        /* AS_SET / AS_CONFED_SET 用 {} 包围 */
        bool is_set = (seg_type == 1 || seg_type == 4);
        if (is_set && wpos < bufsz - 1)
        {
            buf[wpos++] = '{';
        }

        for (uint8_t i = 0; i < seg_len; i++)
        {
            uint32_t asn;
            if (as4)
            {
                asn = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos + 1] << 16) | ((uint32_t)data[pos + 2] << 8) |
                      data[pos + 3];
                pos += 4;
            }
            else
            {
                asn = ((uint32_t)data[pos] << 8) | data[pos + 1];
                pos += 2;
            }

            if (i > 0 && wpos < bufsz - 1)
            {
                buf[wpos++] = is_set ? ',' : ' ';
            }

            int n = snprintf(buf + wpos, bufsz - wpos, "%u", asn);
            if (n > 0)
            {
                wpos += (size_t)n;
            }
        }

        if (is_set && wpos < bufsz - 1)
        {
            buf[wpos++] = '}';
        }
    }

    buf[wpos < bufsz ? wpos : bufsz - 1] = '\0';
}

/* ============================================================================
 * COMMUNITY 解析
 * ========================================================================== */

/** 已知 Well-known community 转字符串 */
static const char *wellknown_community(uint32_t val)
{
    switch (val)
    {
        case 0xFFFFFF01:
            return "no-export";
        case 0xFFFFFF02:
            return "no-advertise";
        case 0xFFFFFF03:
            return "no-export-subconfed";
        case 0x00000000:
            return "internet";
        default:
            return NULL;
    }
}

static void parse_community(const uint8_t *data, uint16_t len, char *buf, size_t bufsz)
{
    size_t wpos = 0;
    for (uint16_t i = 0; i + 4 <= len; i += 4)
    {
        uint32_t val =
            ((uint32_t)data[i] << 24) | ((uint32_t)data[i + 1] << 16) | ((uint32_t)data[i + 2] << 8) | data[i + 3];
        const char *wk = wellknown_community(val);
        int n;

        if (wpos > 0 && wpos < bufsz - 1)
        {
            buf[wpos++] = ' ';
        }

        if (wk)
        {
            n = snprintf(buf + wpos, bufsz - wpos, "%s", wk);
        }
        else
        {
            n = snprintf(buf + wpos, bufsz - wpos, "%u:%u", val >> 16, val & 0xFFFF);
        }

        if (n > 0)
        {
            wpos += (size_t)n;
        }
    }
    buf[wpos < bufsz ? wpos : bufsz - 1] = '\0';
}

/* ============================================================================
 * EXT_COMMUNITY 解析（8 字节）
 * ========================================================================== */

static void parse_ext_community_one(const uint8_t *d, char *buf, size_t bufsz)
{
    uint8_t high = d[0];
    uint8_t sub = d[1];

    /* 判断是否为 Route Target（sub-type 0x02） */
    bool is_rt = (sub == 0x02);

    switch (high & 0x3F) /* 低 6 位为 type */
    {
        case 0x00: /* 2-Octet AS */
        {
            uint16_t as = ((uint16_t)d[2] << 8) | d[3];
            uint32_t val = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
            snprintf(buf, bufsz, "%s:%u:%u", is_rt ? "rt" : "ro", as, val);
            break;
        }
        case 0x01: /* IPv4 Address Specific */
        {
            char ip[INET_ADDRSTRLEN];
            uint32_t raw = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 8) | d[5];
            struct in_addr ia;
            ia.s_addr = htonl(raw);
            inet_ntop(AF_INET, &ia, ip, sizeof(ip));
            uint16_t val2 = ((uint16_t)d[6] << 8) | d[7];
            snprintf(buf, bufsz, "%s:%s:%u", is_rt ? "rt" : "ro", ip, val2);
            break;
        }
        case 0x02: /* 4-Octet AS */
        {
            uint32_t as4 = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 8) | d[5];
            uint16_t val = ((uint16_t)d[6] << 8) | d[7];
            snprintf(buf, bufsz, "%s:%u:%u", is_rt ? "rt" : "ro", as4, val);
            break;
        }
        default:
            snprintf(buf, bufsz, "0x%02x%02x:%02x%02x%02x%02x%02x%02x", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            break;
    }
}

static void parse_ext_communities(const uint8_t *data, uint16_t len, char *buf, size_t bufsz)
{
    size_t wpos = 0;
    for (uint16_t i = 0; i + 8 <= len; i += 8)
    {
        char one[64];
        parse_ext_community_one(data + i, one, sizeof(one));
        if (wpos > 0 && wpos < bufsz - 1)
        {
            buf[wpos++] = ' ';
        }
        int n = snprintf(buf + wpos, bufsz - wpos, "%s", one);
        if (n > 0)
        {
            wpos += (size_t)n;
        }
    }
    buf[wpos < bufsz ? wpos : bufsz - 1] = '\0';
}

/* ============================================================================
 * LARGE_COMMUNITY 解析（12 字节，RFC 8092）
 * ========================================================================== */

static void parse_large_communities(const uint8_t *data, uint16_t len, char *buf, size_t bufsz)
{
    size_t wpos = 0;
    for (uint16_t i = 0; i + 12 <= len; i += 12)
    {
        uint32_t ga =
            ((uint32_t)data[i] << 24) | ((uint32_t)data[i + 1] << 16) | ((uint32_t)data[i + 2] << 8) | data[i + 3];
        uint32_t ld1 =
            ((uint32_t)data[i + 4] << 24) | ((uint32_t)data[i + 5] << 16) | ((uint32_t)data[i + 6] << 8) | data[i + 7];
        uint32_t ld2 = ((uint32_t)data[i + 8] << 24) | ((uint32_t)data[i + 9] << 16) | ((uint32_t)data[i + 10] << 8) |
                       data[i + 11];

        if (wpos > 0 && wpos < bufsz - 1)
        {
            buf[wpos++] = ' ';
        }
        int n = snprintf(buf + wpos, bufsz - wpos, "%u:%u:%u", ga, ld1, ld2);
        if (n > 0)
        {
            wpos += (size_t)n;
        }
    }
    buf[wpos < bufsz ? wpos : bufsz - 1] = '\0';
}

/* ============================================================================
 * 主路径属性解析函数
 * ========================================================================== */

int bgp_parse_path_attrs(const uint8_t *data, uint16_t len, uint32_t flags, bgp_attr_t *attr, bgp_nexthop_t *nexthop,
                         mp_reach_info_t *mp_reach, mp_unreach_info_t *mp_unreach)
{
    bool as4 = (flags & BGP_PARSE_FLAG_AS4) || !(flags); /* 默认 AS4 */
    uint16_t pos = 0;

    while (pos + 3 <= len)
    {
        uint8_t aflags = data[pos++];
        uint8_t type = data[pos++];
        uint16_t attr_len;

        /* Extended-Length bit (bit 4) */
        if (aflags & 0x10)
        {
            if (pos + 2 > len)
            {
                break;
            }
            attr_len = ((uint16_t)data[pos] << 8) | data[pos + 1];
            pos += 2;
        }
        else
        {
            attr_len = data[pos++];
        }

        if (pos + attr_len > len)
        {
            break;
        }

        const uint8_t *val = data + pos;

        switch (type)
        {
            case ATTR_ORIGIN:
                if (attr_len >= 1)
                {
                    attr->origin = (bgp_origin_t)val[0];
                }
                break;

            case ATTR_AS_PATH:
                parse_as_path(val, attr_len, as4, attr->as_path, sizeof(attr->as_path));
                break;

            case ATTR_NEXT_HOP:
                if (attr_len == 4)
                {
                    nexthop->global.family = AF_INET;
                    memcpy(&nexthop->global.u.v4, val, 4);
                }
                break;

            case ATTR_MULTI_EXIT_DISC:
                if (attr_len == 4)
                {
                    attr->med = ((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) | ((uint32_t)val[2] << 8) | val[3];
                    attr->has_med = true;
                }
                break;

            case ATTR_LOCAL_PREF:
                if (attr_len == 4)
                {
                    attr->local_pref =
                        ((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) | ((uint32_t)val[2] << 8) | val[3];
                    attr->has_local_pref = true;
                }
                break;

            case ATTR_ATOMIC_AGGREGATE:
                attr->atomic_aggregate = true;
                break;

            case ATTR_AGGREGATOR:
            {
                /* 2-byte AS: AS(2B)+IP(4B)=6B; 4-byte AS: AS(4B)+IP(4B)=8B */
                char ip[INET_ADDRSTRLEN];
                uint32_t agg_as;
                const uint8_t *ip_ptr;

                if (as4 && attr_len >= 8)
                {
                    agg_as = ((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) | ((uint32_t)val[2] << 8) | val[3];
                    ip_ptr = val + 4;
                }
                else if (attr_len >= 6)
                {
                    agg_as = ((uint32_t)val[0] << 8) | val[1];
                    ip_ptr = val + 2;
                }
                else
                {
                    break;
                }
                struct in_addr ia;
                memcpy(&ia, ip_ptr, 4);
                inet_ntop(AF_INET, &ia, ip, sizeof(ip));
                snprintf(attr->aggregator, sizeof(attr->aggregator), "%u:%s", agg_as, ip);
                break;
            }

            case ATTR_COMMUNITY:
                parse_community(val, attr_len, attr->communities, sizeof(attr->communities));
                break;

            case ATTR_ORIGINATOR_ID:
                if (attr_len == 4)
                {
                    attr->originator_id.family = AF_INET;
                    memcpy(&attr->originator_id.u.v4, val, 4);
                    attr->has_originator_id = true;
                }
                break;

            case ATTR_CLUSTER_LIST:
                if (attr_len > 0 && (attr_len % 4) == 0)
                {
                    uint8_t cap = (uint8_t)(sizeof(attr->cluster_list) / sizeof(attr->cluster_list[0]));
                    uint8_t n = (uint8_t)(attr_len / 4);
                    if (n > cap)
                    {
                        n = cap;
                    }
                    for (uint8_t i = 0; i < n; i++)
                    {
                        attr->cluster_list[i].family = AF_INET;
                        memcpy(&attr->cluster_list[i].u.v4, val + i * 4, 4);
                    }
                    attr->cluster_list_len = n;
                }
                break;

            case ATTR_MP_REACH_NLRI:
            {
                /* AFI(2) + SAFI(1) + NH_LEN(1) + NH + SNPA(1) + NLRI */
                if (attr_len < 4)
                {
                    break;
                }
                uint16_t mp_afi = ((uint16_t)val[0] << 8) | val[1];
                uint8_t mp_safi = val[2];
                uint8_t nh_len = val[3];

                if (4 + nh_len + 1 > attr_len)
                {
                    break;
                }

                mp_reach->afi = mp_afi;
                mp_reach->safi = mp_safi;
                mp_reach->nh_data = val + 4;
                mp_reach->nh_len = nh_len;
                /* SNPA 字节跳过（offset: 4 + nh_len） */
                uint8_t snpa_len = val[4 + nh_len];
                uint16_t nlri_off = (uint16_t)(4 + nh_len + 1 + snpa_len);
                if (nlri_off > attr_len)
                {
                    break;
                }
                mp_reach->nlri_data = val + nlri_off;
                mp_reach->nlri_len = attr_len - nlri_off;
                mp_reach->valid = true;
                break;
            }

            case ATTR_MP_UNREACH_NLRI:
            {
                /* AFI(2) + SAFI(1) + NLRI */
                if (attr_len < 3)
                {
                    break;
                }
                mp_unreach->afi = ((uint16_t)val[0] << 8) | val[1];
                mp_unreach->safi = val[2];
                mp_unreach->nlri_data = val + 3;
                mp_unreach->nlri_len = attr_len - 3;
                mp_unreach->valid = true;
                break;
            }

            case ATTR_EXT_COMMUNITIES:
                parse_ext_communities(val, attr_len, attr->ext_communities, sizeof(attr->ext_communities));
                break;

            case ATTR_LARGE_COMMUNITY:
                parse_large_communities(val, attr_len, attr->large_communities, sizeof(attr->large_communities));
                break;

            /* AS4_PATH / AS4_AGGREGATOR：已通过 AS4 能力处理，忽略 */
            case ATTR_AS4_PATH:
            case ATTR_AS4_AGGREGATOR:
            default:
                break;
        }

        pos += attr_len;
    }

    return 0;
}
