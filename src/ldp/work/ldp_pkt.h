/**
 * @file   ldp_pkt.h
 * @brief  LDP PDU 头部 / Hello / TLV 编解码（RFC 5036 §3）
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_PKT_H
#define LDP_PKT_H

#include <stddef.h>
#include <stdint.h>

/* 协议版本 */
#define LDP_VERSION 1

/* PDU 头部固定长度（version + pdu_length + lsr_id + label_space） */
#define LDP_PDU_HEADER_SIZE 10

/* 消息头部固定长度（U|F+type + length + msg_id） */
#define LDP_MSG_HEADER_SIZE 8

/* 常用消息类型（不带 U bit） */
#define LDP_MSG_TYPE_NOTIFICATION 0x0001
#define LDP_MSG_TYPE_HELLO 0x0100
#define LDP_MSG_TYPE_INITIALIZATION 0x0200
#define LDP_MSG_TYPE_KEEPALIVE 0x0201
#define LDP_MSG_TYPE_ADDRESS 0x0300
#define LDP_MSG_TYPE_ADDRESS_WITHDRAW 0x0301
#define LDP_MSG_TYPE_LABEL_MAPPING 0x0400
#define LDP_MSG_TYPE_LABEL_REQUEST 0x0401
#define LDP_MSG_TYPE_LABEL_WITHDRAW 0x0402
#define LDP_MSG_TYPE_LABEL_RELEASE 0x0403
#define LDP_MSG_TYPE_LABEL_ABORT_REQUEST 0x0404

/* 常用 TLV 类型 */
#define LDP_TLV_FEC 0x0100
#define LDP_TLV_ADDRESS_LIST 0x0101
#define LDP_TLV_GENERIC_LABEL 0x0200
#define LDP_TLV_COMMON_HELLO_PARAMS 0x0400
#define LDP_TLV_IPV4_TRANSPORT_ADDR 0x0401
#define LDP_TLV_CONFIG_SEQ_NUMBER 0x0402
#define LDP_TLV_COMMON_SESSION_PARAMS 0x0500

/* FEC 元素类型（RFC 5036 §3.4.1） */
#define LDP_FEC_ELEM_WILDCARD 0x01
#define LDP_FEC_ELEM_PREFIX 0x02
#define LDP_FEC_ELEM_HOST 0x03

/* Address Family Numbers（IANA） */
#define LDP_AFNUM_IPV4 1
#define LDP_AFNUM_IPV6 2

/* Reserved labels */
#define LDP_LABEL_IPV4_EXPLICIT_NULL 0u
#define LDP_LABEL_ROUTER_ALERT 1u
#define LDP_LABEL_IPV6_EXPLICIT_NULL 2u
#define LDP_LABEL_IMPLICIT_NULL 3u

/**
 * @brief LDP PDU 头部解析结果
 */
typedef struct ldp_pdu_hdr
{
    uint16_t version;
    uint16_t pdu_length;  /**< 协议头声明的剩余长度（不含 version+length 字段自身） */
    uint32_t lsr_id;      /**< host order */
    uint16_t label_space; /**< host order */
} ldp_pdu_hdr_t;

/**
 * @brief LDP 消息头部解析结果
 */
typedef struct ldp_msg_hdr
{
    uint8_t u_bit;
    uint16_t msg_type;   /**< 已去掉 U bit */
    uint16_t msg_length; /**< 不含 type+length 自身 */
    uint32_t msg_id;
} ldp_msg_hdr_t;

/**
 * @brief Hello 消息解析后字段
 */
typedef struct ldp_hello_info
{
    uint8_t valid;            /**< 1=解析成功 */
    uint8_t targeted;         /**< Common Hello Params T-bit */
    uint8_t request_targeted; /**< Common Hello Params R-bit */
    uint16_t hold_time_sec;   /**< 0xFFFF 表示无穷；0 表示使用默认 */
    uint32_t transport_v4;    /**< host order；0 表示未携带 TLV */
    uint32_t configuration_seq;
} ldp_hello_info_t;

/**
 * @brief 编码一个完整的 Hello PDU
 *
 * 输出格式：
 *   PDU 头(10) + Msg 头(8) + Common Hello Params TLV(8) +
 *   IPv4 Transport Addr TLV(8) + Configuration Seq Num TLV(8)
 *
 * @param self_lsr_id      本端 LSR-ID (host order)
 * @param label_space      本端 label space
 * @param msg_id           消息 ID
 * @param hold_time_sec    Common Hello Params hold time（秒）
 * @param transport_v4     IPv4 Transport Address (host order)；0 表示不携带该 TLV
 * @param config_seq       Configuration Sequence Number
 * @param out              输出缓冲区
 * @param out_cap          缓冲区容量
 * @return 实际写入字节数；-1 表示缓冲区不足
 */
int ldp_pkt_encode_hello(uint32_t self_lsr_id, uint16_t label_space, uint32_t msg_id, uint16_t hold_time_sec,
                         uint32_t transport_v4, uint32_t config_seq, uint8_t *out, size_t out_cap);

/**
 * @brief 解析 PDU 头部
 * @return 0 成功，-1 长度不足或版本/长度字段非法
 */
int ldp_pkt_parse_pdu_hdr(const uint8_t *buf, size_t buf_len, ldp_pdu_hdr_t *hdr_out);

/**
 * @brief 在已解析 PDU 头之后，解析下一个消息头
 * @param buf            消息头起始位置
 * @param buf_len        剩余长度
 * @return 0 成功，-1 长度不足
 */
int ldp_pkt_parse_msg_hdr(const uint8_t *buf, size_t buf_len, ldp_msg_hdr_t *hdr_out);

/**
 * @brief 解析 Hello 消息体内 TLV 序列
 * @param body         消息体起始（去掉消息头）
 * @param body_len     消息体长度（即 msg_length - 4）
 * @param info_out     输出
 * @return 0 成功，-1 必选 TLV 缺失或解析失败
 */
int ldp_pkt_parse_hello(const uint8_t *body, size_t body_len, ldp_hello_info_t *info_out);

/**
 * @brief Initialization 消息解析后字段（仅含 Common Session Parameters TLV）
 */
typedef struct ldp_init_info
{
    uint8_t valid;
    uint16_t protocol_version;
    uint16_t keepalive_time_sec;
    uint8_t a_bit;  /**< loop detection */
    uint8_t d_bit;  /**< downstream-on-demand */
    uint16_t pvlim; /**< Path Vector Limit */
    uint16_t max_pdu_len;
    uint32_t recv_lsr_id;
    uint16_t recv_label_space;
} ldp_init_info_t;

/**
 * @brief 编码 Initialization PDU
 *
 * 输出格式：PDU 头(10) + Msg 头(8) + Common Session Params TLV(18 = 4+14)
 *
 * @param self_lsr_id      本端 LSR-ID (host order)
 * @param self_label_space 本端 label space
 * @param msg_id           消息 ID
 * @param keepalive_sec    本端通告 keepalive 时间（秒）
 * @param recv_lsr_id      接收端 LSR-ID（即对端的 LSR-ID, host order）
 * @param recv_label_space 接收端 label space
 * @param out              输出缓冲
 * @param out_cap          缓冲容量
 * @return 实际写入字节数；-1 表示失败
 */
int ldp_pkt_encode_init(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint16_t keepalive_sec,
                        uint32_t recv_lsr_id, uint16_t recv_label_space, uint8_t *out, size_t out_cap);

/**
 * @brief 编码 KeepAlive PDU（消息体为空）
 */
int ldp_pkt_encode_keepalive(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint8_t *out,
                             size_t out_cap);

/**
 * @brief 解析 Initialization 消息体（去掉消息头后）
 * @return 0 成功，-1 失败
 */
int ldp_pkt_parse_init(const uint8_t *body, size_t body_len, ldp_init_info_t *info_out);

// ============================================================================
// Address / Address Withdraw 消息（仅 IPv4，单 TLV）
// ============================================================================

/**
 * @brief 编码 Address / Address Withdraw 消息
 * @param withdraw 0=Address, 1=Address Withdraw
 * @param addrs    本端 IPv4 地址数组（host order）
 * @param n_addrs  地址数量
 */
int ldp_pkt_encode_address(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, int withdraw,
                           const uint32_t *addrs, size_t n_addrs, uint8_t *out, size_t out_cap);

/**
 * @brief 解析 Address List TLV，输出 IPv4 地址数组（host order）
 * @return 实际解析出的地址数；-1 失败
 */
int ldp_pkt_parse_address_list_tlv(const uint8_t *body, size_t body_len, uint32_t *addrs_out, size_t cap);

// ============================================================================
// Label Mapping / Withdraw / Release（IPv4 Prefix FEC + Generic Label）
// ============================================================================

/**
 * @brief 编码 Label Mapping
 * @param fec_prefix    IPv4 前缀（host order）
 * @param fec_prefix_len 前缀长度（0-32）
 * @param label         Generic Label（20-bit）
 */
int ldp_pkt_encode_label_mapping(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint32_t fec_prefix,
                                 uint8_t fec_prefix_len, uint32_t label, uint8_t *out, size_t out_cap);

/**
 * @brief 编码 Label Withdraw
 */
int ldp_pkt_encode_label_withdraw(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint32_t fec_prefix,
                                  uint8_t fec_prefix_len, uint8_t *out, size_t out_cap);

/**
 * @brief 解析 Label Mapping/Withdraw 消息体
 * @param fec_prefix_out / fec_prefix_len_out 输出第一条 IPv4 prefix FEC
 * @param label_out 输出 generic label；若不存在置 0
 * @return 0 成功，-1 必选 TLV 缺失或解析失败
 */
int ldp_pkt_parse_label_msg(const uint8_t *body, size_t body_len, uint32_t *fec_prefix_out, uint8_t *fec_prefix_len_out,
                            uint32_t *label_out);

#endif /* LDP_PKT_H */
