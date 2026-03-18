/**
 * @file   bgp_pkt_build_ipv4uc.c
 * @brief  BGP UPDATE 报文 IPv4 单播（AFI=1, SAFI=1）AF 编码器
 * @author jhb
 * @date   2026/03/15
 */
#include <arpa/inet.h>
#include <string.h>

#include "bgp_pkt_build.h"

// ============================================================================
// IPv4 单播编码器实现
// ============================================================================

/**
 * @brief 编码 NEXT_HOP 路径属性（IPv4，4 字节）
 */
static int encode_nexthop_attr(uint8_t *buf, int buf_size, const net_addr_t *nh)
{
    if (nh->family != AF_INET)
    {
        return 0; /* 非 IPv4 地址，跳过 */
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
 * @brief 宣告 PA：NEXT_HOP 属性
 */
static int ipv4uc_encode_reach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                  const bgp_nexthop_t *nexthop)
{
    (void)nlri;
    return encode_nexthop_attr(buf, buf_size, &nexthop->global);
}

/**
 * @brief 宣告 NLRI：前缀写入 NLRI 字段
 */
static int ipv4uc_encode_reach_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
{
    return bgp_pkt_encode_prefix(buf, buf_size, &nlri->prefix.prefix);
}

/**
 * @brief 撤销 Withdrawn Routes：前缀写入 Withdrawn Routes 字段
 */
static int ipv4uc_encode_unreach_wd(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
{
    return bgp_pkt_encode_prefix(buf, buf_size, &nlri->prefix.prefix);
}

/**
 * @brief 撤销 PA：IPv4 撤销无额外路径属性
 */
static int ipv4uc_encode_unreach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
{
    (void)buf;
    (void)buf_size;
    (void)nlri;
    return 0;
}

/** IPv4 单播编码器描述符 */
static const bgp_pkt_af_enc_t g_ipv4uc_enc = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_UNICAST,
    .encode_reach_pa = ipv4uc_encode_reach_pa,
    .encode_reach_nlri = ipv4uc_encode_reach_nlri,
    .encode_unreach_wd = ipv4uc_encode_unreach_wd,
    .encode_unreach_pa = ipv4uc_encode_unreach_pa,
};

void bgp_pkt_build_ipv4uc_register(void)
{
    bgp_pkt_af_enc_register(&g_ipv4uc_enc);
}
