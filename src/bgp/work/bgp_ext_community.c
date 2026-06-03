/**
 * @file   bgp_ext_community.c
 * @brief  BGP Extended Community raw buffer helpers
 */
#include "bgp_ext_community.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "vrf.h"

static uint16_t vrf_afi_from_bgp(uint16_t afi)
{
    return (afi == BGP_AFI_IPV6) ? VRF_AFI_IPV6 : VRF_AFI_IPV4;
}

static gboolean ext_community_exists(const bgp_attr_t *attr, const uint8_t ext[8])
{
    if (!attr || !ext || (attr->ext_communities_len % 8) != 0)
    {
        return FALSE;
    }
    for (uint16_t off = 0; off + 8 <= attr->ext_communities_len; off = (uint16_t)(off + 8))
    {
        if (memcmp(attr->ext_communities + off, ext, 8) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean bgp_ext_community_rt_canon(const vrf_rt_t *rt, uint8_t ext[8])
{
    if (!rt || !ext)
    {
        return FALSE;
    }

    const uint8_t *b = rt->bytes;
    memset(ext, 0, 8);

    if (b[0] == 0x00 && b[1] == 0x00)
    {
        ext[0] = 0x00; /* 2-octet AS specific */
        ext[1] = 0x02; /* Route Target */
        memcpy(ext + 2, b + 2, 6);
        return TRUE;
    }
    if (b[0] == 0x00 && b[1] == 0x01)
    {
        ext[0] = 0x01; /* IPv4 address specific */
        ext[1] = 0x02; /* Route Target */
        memcpy(ext + 2, b + 2, 6);
        return TRUE;
    }
    if ((b[0] & 0x3Fu) == 0x02 && b[1] == 0x02)
    {
        memcpy(ext, b, 8);
        return TRUE;
    }
    return FALSE;
}

gboolean bgp_ext_community_is_rt(const uint8_t entry[8])
{
    if (!entry)
    {
        return FALSE;
    }
    /* Route Target 子类型固定为 0x02；主类型可为 2-octet AS / IPv4 / 4-octet AS
     * 的传输或非传输形式((entry[0] & 0x3F) ∈ {0x00,0x01,0x02})。 */
    uint8_t kind = (uint8_t)(entry[0] & 0x3Fu);
    return (entry[1] == 0x02 && (kind == 0x00 || kind == 0x01 || kind == 0x02)) ? TRUE : FALSE;
}

void bgp_ext_community_merge_vrf_export_rts(bgp_attr_t *attr, uint32_t vrf_id, uint16_t afi)
{
    if (!attr || vrf_id == VRF_PUBLIC_VRF_ID)
    {
        return;
    }

    const vrf_api_af_t *af = vrf_api_cache_get_af(vrf_id, vrf_afi_from_bgp(afi), VRF_SAFI_UNICAST);
    if (!af || !af->export_rts || af->export_rt_count == 0)
    {
        return;
    }

    for (uint16_t i = 0; i < af->export_rt_count; i++)
    {
        uint8_t ext[8];
        if (!bgp_ext_community_rt_canon(&af->export_rts[i], ext) || ext_community_exists(attr, ext))
        {
            continue;
        }
        if ((size_t)attr->ext_communities_len + sizeof(ext) > sizeof(attr->ext_communities))
        {
            LOG_WARN("BGP: VRF export RT list truncated while building ext-community vrf=%u afi=%u", vrf_id,
                     (unsigned)afi);
            return;
        }
        memcpy(attr->ext_communities + attr->ext_communities_len, ext, sizeof(ext));
        attr->ext_communities_len = (uint16_t)(attr->ext_communities_len + sizeof(ext));
    }
}

static void format_one(const uint8_t *d, char *buf, size_t bufsz)
{
    uint8_t sub = d[1];
    const char *kind = (sub == 0x02) ? "rt" : ((sub == 0x03) ? "ro" : "ec");

    switch (d[0] & 0x3Fu)
    {
        case 0x00:
        {
            uint16_t asn = ((uint16_t)d[2] << 8) | d[3];
            uint32_t val = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
            snprintf(buf, bufsz, "%s:%u:%u", kind, (unsigned)asn, val);
            break;
        }
        case 0x01:
        {
            char ip[INET_ADDRSTRLEN];
            struct in_addr ia;
            memcpy(&ia.s_addr, d + 2, 4);
            inet_ntop(AF_INET, &ia, ip, sizeof(ip));
            uint16_t val = ((uint16_t)d[6] << 8) | d[7];
            snprintf(buf, bufsz, "%s:%s:%u", kind, ip, (unsigned)val);
            break;
        }
        case 0x02:
        {
            uint32_t asn = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 8) | d[5];
            uint16_t val = ((uint16_t)d[6] << 8) | d[7];
            snprintf(buf, bufsz, "%s:%u:%u", kind, asn, (unsigned)val);
            break;
        }
        default:
            snprintf(buf, bufsz, "0x%02x%02x:%02x%02x%02x%02x%02x%02x", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            break;
    }
}

void bgp_ext_community_format(const uint8_t *data, uint16_t len, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
    {
        return;
    }
    buf[0] = '\0';
    if (!data || len == 0 || (len % 8) != 0)
    {
        return;
    }

    size_t wpos = 0;
    for (uint16_t i = 0; i + 8 <= len; i = (uint16_t)(i + 8))
    {
        char one[64];
        format_one(data + i, one, sizeof(one));
        if (wpos > 0 && wpos < bufsz - 1)
        {
            buf[wpos++] = ' ';
        }
        if (wpos >= bufsz - 1)
        {
            break;
        }
        int n = snprintf(buf + wpos, bufsz - wpos, "%s", one);
        if (n > 0)
        {
            wpos += (size_t)n;
            if (wpos >= bufsz - 1)
            {
                break;
            }
        }
    }
    buf[wpos < bufsz ? wpos : bufsz - 1] = '\0';
}
