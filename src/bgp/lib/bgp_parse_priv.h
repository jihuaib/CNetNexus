/**
 * @file   bgp_parse_priv.h
 * @brief  bgp_parse 库内部私有定义，不对外暴露
 * @author jhb
 * @date   2026/03/11
 */
#ifndef BGP_PARSE_PRIV_H
#define BGP_PARSE_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "bgp.h"

/* ============================================================================
 * 路径属性解析中间结果
 * ========================================================================== */

/**
 * @brief MP_REACH_NLRI（type 14）解析暂存
 */
typedef struct mp_reach_info
{
    uint16_t afi;
    uint8_t safi;
    const uint8_t *nh_data; /**< nexthop 字节指针（指向原始 body，非拷贝） */
    uint8_t nh_len;
    const uint8_t *nlri_data; /**< NLRI 字节指针（SNPA 已跳过） */
    uint16_t nlri_len;
    bool valid;
} mp_reach_info_t;

/**
 * @brief MP_UNREACH_NLRI（type 15）解析暂存
 */
typedef struct mp_unreach_info
{
    uint16_t afi;
    uint8_t safi;
    const uint8_t *nlri_data;
    uint16_t nlri_len;
    bool valid;
} mp_unreach_info_t;

/** 路径属性解析结果 */
typedef enum bgp_path_attr_parse_result
{
    BGP_PATH_ATTR_PARSE_OK = 0,
    BGP_PATH_ATTR_PARSE_TREAT_AS_WITHDRAW = 1,
    BGP_PATH_ATTR_PARSE_ERROR = -1,
} bgp_path_attr_parse_result_t;

/** Prefix-SID 属性自身的 RFC 8669 / RFC 9252 错误动作 */
typedef enum bgp_prefix_sid_parse_result
{
    BGP_PREFIX_SID_PARSE_OK = 0,
    BGP_PREFIX_SID_PARSE_ATTRIBUTE_DISCARD = 1,
    BGP_PREFIX_SID_PARSE_TREAT_AS_WITHDRAW = 2,
} bgp_prefix_sid_parse_result_t;

/* ============================================================================
 * 内部函数声明
 * ========================================================================== */

/**
 * @brief 解析路径属性区域，填充 attr/nexthop/MP_REACH/MP_UNREACH
 */
int bgp_parse_path_attrs(const uint8_t *data, uint16_t len, uint32_t flags, bgp_attr_t *attr, bgp_nexthop_t *nexthop,
                         mp_reach_info_t *mp_reach, mp_unreach_info_t *mp_unreach);

/**
 * @brief 严格解析 Prefix-SID attribute value 并保存 raw value
 * @return bgp_prefix_sid_parse_result_t
 */
int bgp_parse_prefix_sid_value(const uint8_t *data, uint16_t len, uint8_t attr_flags, bgp_attr_t *attr);

/**
 * @brief 各 AF 处理器注册入口（由 bgp_parse_init() 调用）
 */
void bgp_parse_ipv4uc_register(void);
void bgp_parse_ipv6uc_register(void);
void bgp_parse_ipv4qp_register(void);
void bgp_parse_ipv6qp_register(void);
void bgp_parse_evpn_register(void);
void bgp_parse_flowspec_register(void);
void bgp_parse_vpn_register(void);
void bgp_parse_labeled_register(void);

/* ============================================================================
 * 公共辅助函数
 * ========================================================================== */

/**
 * @brief 将 bgp_rd_t 转为可读字符串
 *        Type 0: "ASN:value", Type 1: "IP:value", Type 2: "ASN:value"
 * @param rd  RD 指针
 * @param buf 输出缓冲区（建议 48 字节）
 * @param sz  缓冲区大小
 */
void bgp_rd_to_str(const bgp_rd_t *rd, char *buf, size_t sz);

/**
 * @brief 将 bgp_esi_t 转为 "xx:xx:xx:xx:xx:xx:xx:xx:xx:xx" 格式
 * @param esi ESI 指针
 * @param buf 输出缓冲区（建议 32 字节）
 * @param sz  缓冲区大小
 */
void bgp_esi_to_str(const bgp_esi_t *esi, char *buf, size_t sz);

/**
 * @brief 从 NLRI 字节流中读取一个 IP 前缀（BGP 标准前缀编码）
 * @param data     字节流起始
 * @param avail    可用字节数
 * @param af       AF_INET 或 AF_INET6
 * @param prefix   输出前缀
 * @param consumed 输出：本次消耗字节数
 * @return 0=成功, -1=格式错误
 */
int bgp_read_prefix(const uint8_t *data, uint16_t avail, int af, net_prefix_t *prefix, uint16_t *consumed);

#endif /* BGP_PARSE_PRIV_H */
