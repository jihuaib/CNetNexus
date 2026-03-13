/**
 * @file   bgp_parse_common.c
 * @brief  bgp_parse 库公共辅助函数（RD/ESI 格式化、前缀读取）
 * @author jhb
 * @date   2026/03/11
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "bgp_parse_priv.h"

/* ============================================================================
 * RD 转字符串
 * ========================================================================== */

void bgp_rd_to_str(const bgp_rd_t *rd, char *buf, size_t sz)
{
    const uint8_t *b = rd->bytes;

    /* RD 类型由高 2 字节决定 */
    uint16_t rd_type = ((uint16_t)b[0] << 8) | b[1];

    switch (rd_type)
    {
        case 0: /* Type 0: 2-byte AS + 4-byte value */
        {
            uint16_t as = ((uint16_t)b[2] << 8) | b[3];
            uint32_t val = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
            snprintf(buf, sz, "%u:%u", as, val);
            break;
        }
        case 1: /* Type 1: IPv4 + 2-byte value */
        {
            char ip[INET_ADDRSTRLEN];
            struct in_addr ia;
            memcpy(&ia, b + 2, 4);
            inet_ntop(AF_INET, &ia, ip, sizeof(ip));
            uint16_t val = ((uint16_t)b[6] << 8) | b[7];
            snprintf(buf, sz, "%s:%u", ip, val);
            break;
        }
        case 2: /* Type 2: 4-byte AS + 2-byte value */
        {
            uint32_t as4 = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];
            uint16_t val = ((uint16_t)b[6] << 8) | b[7];
            snprintf(buf, sz, "%u:%u", as4, val);
            break;
        }
        default:
            snprintf(buf, sz, "%02x%02x:%02x%02x:%02x%02x:%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            break;
    }
}

/* ============================================================================
 * ESI 转字符串
 * ========================================================================== */

void bgp_esi_to_str(const bgp_esi_t *esi, char *buf, size_t sz)
{
    const uint8_t *b = esi->bytes;
    snprintf(buf, sz, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6],
             b[7], b[8], b[9]);
}

/* ============================================================================
 * 前缀读取
 * ========================================================================== */

int bgp_read_prefix(const uint8_t *data, uint16_t avail, int af, net_prefix_t *prefix, uint16_t *consumed)
{
    if (avail < 1)
    {
        return -1;
    }

    uint8_t plen = data[0];
    uint8_t max_len = (af == AF_INET6) ? 128 : 32;

    if (plen > max_len)
    {
        return -1;
    }

    /* 需要的字节数 */
    uint8_t nbytes = (plen + 7) / 8;
    if (1 + nbytes > avail)
    {
        return -1;
    }

    memset(prefix, 0, sizeof(*prefix));
    prefix->addr.family = (sa_family_t)af;
    prefix->prefix_len = plen;

    if (af == AF_INET6)
    {
        memcpy(prefix->addr.u.v6.s6_addr, data + 1, nbytes);
        /* 末字节低位补零，避免比较时产生歧义 */
        if (plen % 8 != 0)
        {
            prefix->addr.u.v6.s6_addr[nbytes - 1] &= (uint8_t)(0xFF << (8 - (plen % 8)));
        }
    }
    else
    {
        uint8_t tmp[4] = {0};
        memcpy(tmp, data + 1, nbytes);
        if (plen % 8 != 0)
        {
            tmp[nbytes - 1] &= (uint8_t)(0xFF << (8 - (plen % 8)));
        }
        memcpy(&prefix->addr.u.v4, tmp, 4);
    }

    *consumed = (uint16_t)(1 + nbytes);
    return 0;
}
