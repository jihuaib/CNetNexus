/**
 * @file   bgp_parse.h
 * @brief  BGP UPDATE 报文解析器公共 API 及 AFI/SAFI 处理器注册接口
 * @author jhb
 * @date   2026/03/11
 */
#ifndef BGP_PARSE_H
#define BGP_PARSE_H

#include <stdbool.h>
#include <stdint.h>

#include "bgp_msg.h"
#include "bgp_route.h"

/* ============================================================================
 * 解析标志
 * ========================================================================== */

/** 使用 4 字节 AS 号（RFC 6793），默认开启 */
#define BGP_PARSE_FLAG_AS4 (1u << 0)

/* ============================================================================
 * AFI/SAFI 处理器注册接口
 * ========================================================================== */

/**
 * @brief AFI/SAFI NLRI 处理器描述符
 *
 * 各 AF 模块（bgp_parse_ipv4uc.c 等）实现并通过 bgp_af_parser_register()
 * 注册。外部模块也可注册自定义处理器以扩展新的 AFI/SAFI 支持。
 */
typedef struct bgp_af_parser
{
    uint16_t afi; /**< 地址族（1=IPv4, 2=IPv6, 25=L2VPN, ...） */
    uint8_t safi; /**< 子地址族（1=unicast, 4=labeled, 70=EVPN, 128=VPN, 133=FlowSpec） */

    /**
     * @brief 解析 MP_REACH NLRI 字节流，输出 NLRI 条目数组
     * @param data    NLRI 字节（SNPA 已跳过，直接为前缀数据）
     * @param len     字节长度
     * @param out     输出条目数组（内部 malloc，调用者 free）
     * @param out_len 输出条目数量
     * @return 0=成功, -1=解析错误
     */
    int (*parse_reach)(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len);

    /**
     * @brief 解析 MP_UNREACH NLRI 字节流，输出撤销条目数组
     * @param data    NLRI 字节
     * @param len     字节长度
     * @param out     输出条目数组（内部 malloc，调用者 free）
     * @param out_len 输出条目数量
     * @return 0=成功, -1=解析错误
     */
    int (*parse_unreach)(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len);

    /**
     * @brief 解析 MP_REACH nexthop 字节（格式因 AFI 而异）
     * @param nh_data nexthop 字节
     * @param nh_len  字节长度
     * @param nexthop 输出 nexthop
     * @return 0=成功, -1=格式错误
     */
    int (*parse_nexthop)(const uint8_t *nh_data, uint8_t nh_len, bgp_nexthop_t *nexthop);

    /**
     * @brief 填充 entry->key 字符串（唯一键，供哈希表和显示使用）
     * @param entry 目标条目（in/out）
     */
    void (*entry_to_str)(bgp_nlri_entry_t *entry);
} bgp_af_parser_t;

/**
 * @brief 注册 AFI/SAFI 处理器
 * @param parser 处理器描述符（生命周期须长于库）
 * @return 0=成功, -1=失败
 */
int bgp_af_parser_register(const bgp_af_parser_t *parser);

/**
 * @brief 查找已注册的 AFI/SAFI 处理器
 * @param afi  地址族
 * @param safi 子地址族
 * @return 处理器指针，NULL=未注册
 */
const bgp_af_parser_t *bgp_af_parser_find(uint16_t afi, uint8_t safi);

/* ============================================================================
 * UPDATE 解析主 API
 * ========================================================================== */

/**
 * @brief 解析 BGP UPDATE 报文体（不含 19 字节 BGP header）
 *
 * 调用者须通过 bgp_update_result_free() 释放 *result。
 * 函数返回 -1 时 *result 为 NULL。
 *
 * @param body     报文体起始地址（从 Withdrawn Routes Length 字段开始）
 * @param body_len 报文体长度（字节）
 * @param flags    解析标志（BGP_PARSE_FLAG_*），0 = 默认（AS4 开启）
 * @param result   输出结果（内部分配）
 * @return 0=成功, -1=解析错误
 */
int bgp_update_parse(const uint8_t *body, uint32_t body_len, uint32_t flags, bgp_update_result_t **result);

/**
 * @brief 释放 UPDATE 解析结果（NULL 安全）
 * @param result 待释放结果
 */
void bgp_update_result_free(bgp_update_result_t *result);

/**
 * @brief 初始化 BGP parse 库，注册所有内置 AFI/SAFI 处理器
 *
 * 在首次调用 bgp_update_parse() 之前必须调用一次；多次调用幂等。
 */
void bgp_parse_init(void);

/* ============================================================================
 * BGP PDU Header 解析
 * ========================================================================== */

/**
 * @brief 解析 BGP PDU header（Marker + Length + Type），验证合法性
 *
 * @param data     PDU 起始地址
 * @param data_len 可用字节数
 * @param info     输出解析结果（body 指针指向原始缓冲区，非拷贝）
 * @return 0=成功, -1=数据不足或格式错误
 */
int bgp_pdu_parse_hdr(const uint8_t *data, uint32_t data_len, bgp_pdu_info_t *info);

/* ============================================================================
 * BGP OPEN 解析
 * ========================================================================== */

/**
 * @brief 解析 BGP OPEN 报文体（不含 19 字节 header）
 *
 * @param body     报文体起始地址（version 字节开始）
 * @param body_len 报文体长度
 * @param msg      输出解析结果
 * @return 0=成功, -1=格式错误
 */
int bgp_open_parse(const uint8_t *body, uint16_t body_len, bgp_open_msg_t *msg);

/* ============================================================================
 * BGP NOTIFICATION 解析
 * ========================================================================== */

/**
 * @brief 解析 BGP NOTIFICATION 报文体（不含 19 字节 header）
 *
 * @param body     报文体起始地址（error_code 字节开始）
 * @param body_len 报文体长度
 * @param msg      输出解析结果
 * @return 0=成功（即使格式不完整也尽力解析）, -1=body 为 NULL
 */
int bgp_notif_parse(const uint8_t *body, uint16_t body_len, bgp_notif_msg_t *msg);

/**
 * @brief 返回 NOTIFICATION 错误码的可读字符串
 *
 * @param code    Error Code
 * @param subcode Error Subcode
 * @return 静态字符串，如 "Cease/Admin-Shutdown"
 */
const char *bgp_notif_error_str(uint8_t code, uint8_t subcode);

#endif /* BGP_PARSE_H */
