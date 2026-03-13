/**
 * @file   sbmp_bmp.h
 * @brief  BMP 报文解析辅助（纯解析，无状态）
 * @author jhb
 * @date   2026/03/13
 */
#ifndef SBMP_BMP_H
#define SBMP_BMP_H

#include <stdint.h>

/** BMP common header 长度 */
#define SBMP_BMP_COMMON_HDR_LEN 6
/** BMP per-peer header 长度 */
#define SBMP_BMP_PEER_HDR_LEN 42
/** BMP 报文最大长度（common header length 字段上限） */
#define SBMP_BMP_MSG_LEN_MAX 65535

/** BMP message type（RFC 7854） */
#define SBMP_BMP_MSG_ROUTE_MONITORING 0
#define SBMP_BMP_MSG_STATS_REPORT 1
#define SBMP_BMP_MSG_PEER_DOWN 2
#define SBMP_BMP_MSG_PEER_UP 3
#define SBMP_BMP_MSG_INITIATION 4
#define SBMP_BMP_MSG_TERMINATION 5
#define SBMP_BMP_MSG_ROUTE_MIRRORING 6

/** BMP peer type（RFC 9069） */
#define SBMP_BMP_PEER_TYPE_GLOBAL 0
#define SBMP_BMP_PEER_TYPE_RD_INSTANCE 1
#define SBMP_BMP_PEER_TYPE_LOCAL_INSTANCE 2
#define SBMP_BMP_PEER_TYPE_LOC_RIB 3

/** peer 地址字符串最大长度（兼容 IPv6 文本） */
#define SBMP_BMP_PEER_IP_MAX 64
/** BGP ID 字符串最大长度 */
#define SBMP_BMP_BGP_ID_MAX 16

/**
 * @brief BMP common header 视图
 */
typedef struct sbmp_bmp_common_view
{
    uint8_t version;
    uint32_t msg_len;
    uint8_t msg_type;
} sbmp_bmp_common_view_t;

/**
 * @brief BMP per-peer header 解析视图
 */
typedef struct sbmp_bmp_peer_view
{
    char peer_ip[SBMP_BMP_PEER_IP_MAX];
    uint32_t peer_as;
    char bgp_id[SBMP_BMP_BGP_ID_MAX];
    uint8_t peer_type;
    uint8_t is_ipv6;
    uint8_t is_post_policy; /**< 1=post-policy, 0=pre-policy */
    uint8_t is_loc_rib;
    uint8_t is_loc_rib_filtered; /**< 仅 peer-type=3 时有效 */
} sbmp_bmp_peer_view_t;

/**
 * @brief 解析 BMP common header
 * @param msg     报文起始地址
 * @param msg_len 可用字节数
 * @param out     输出
 * @return 0 成功，-1 失败
 */
int sbmp_bmp_parse_common_header(const uint8_t *msg, uint32_t msg_len, sbmp_bmp_common_view_t *out);

/**
 * @brief 解析 BMP per-peer header
 * @param peer_hdr per-peer header 起始地址
 * @param hdr_len  可用字节数
 * @param out      输出
 * @return 0 成功，-1 失败
 */
int sbmp_bmp_parse_peer_header(const uint8_t *peer_hdr, uint32_t hdr_len, sbmp_bmp_peer_view_t *out);

/**
 * @brief 解析 Route Monitoring 报文并提取 BGP PDU 负载视图
 *
 * 要求输入至少包含 common header + per-peer header。
 */
int sbmp_bmp_extract_route_monitoring(const uint8_t *msg, uint32_t msg_len, sbmp_bmp_peer_view_t *peer_out,
                                      const uint8_t **bgp_pdu, uint32_t *bgp_pdu_len);

#endif /* SBMP_BMP_H */
