/**
 * @file   ldp_pkt.c
 * @brief  LDP PDU 编解码实现
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_pkt.h"

#include <arpa/inet.h>
#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int ldp_pkt_encode_hello(uint32_t self_lsr_id, uint16_t label_space, uint32_t msg_id, uint16_t hold_time_sec,
                         uint32_t transport_v4, uint32_t config_seq, uint8_t *out, size_t out_cap)
{
    if (!out)
    {
        return -1;
    }

    /* TLV 段大小：
     *   Common Hello Params: 4 bytes header + 4 bytes value = 8
     *   IPv4 Transport TLV : 4 + 4 = 8 (可选，仅当 transport_v4!=0 携带)
     *   Config Seq Num     : 4 + 4 = 8
     */
    int has_xport = (transport_v4 != 0u) ? 1 : 0;
    size_t tlvs_len = 8u + (has_xport ? 8u : 0u) + 8u;
    size_t msg_body_len = tlvs_len; /* msg_id 已在 msg 头里 */
    size_t msg_total = LDP_MSG_HEADER_SIZE + msg_body_len;
    /* PDU length 字段 = 自 LSR-ID 起到结尾的字节数（即 6 + msg_total） */
    size_t pdu_payload = 6u + msg_total;
    size_t pdu_total = LDP_PDU_HEADER_SIZE + msg_total;

    if (out_cap < pdu_total)
    {
        return -1;
    }

    uint8_t *p = out;

    /* PDU 头 */
    put_u16(p, LDP_VERSION);
    p += 2;
    put_u16(p, (uint16_t)pdu_payload);
    p += 2;
    put_u32(p, self_lsr_id);
    p += 4;
    put_u16(p, label_space);
    p += 2;

    /* Msg 头：U=0, type=Hello, length=msg_body_len+4(msg_id) ... 实际上 length 字段
     * 不含 type/length 自身，但包含 msg_id */
    put_u16(p, LDP_MSG_TYPE_HELLO);
    p += 2;
    put_u16(p, (uint16_t)(4u + msg_body_len));
    p += 2;
    put_u32(p, msg_id);
    p += 4;

    /* Common Hello Parameters TLV */
    put_u16(p, LDP_TLV_COMMON_HELLO_PARAMS);
    p += 2;
    put_u16(p, 4u);
    p += 2;
    put_u16(p, hold_time_sec);
    p += 2;
    put_u16(p, 0u); /* T=0, R=0, reserved */
    p += 2;

    if (has_xport)
    {
        put_u16(p, LDP_TLV_IPV4_TRANSPORT_ADDR);
        p += 2;
        put_u16(p, 4u);
        p += 2;
        put_u32(p, transport_v4);
        p += 4;
    }

    /* Configuration Sequence Number */
    put_u16(p, LDP_TLV_CONFIG_SEQ_NUMBER);
    p += 2;
    put_u16(p, 4u);
    p += 2;
    put_u32(p, config_seq);
    p += 4;

    return (int)(p - out);
}

int ldp_pkt_parse_pdu_hdr(const uint8_t *buf, size_t buf_len, ldp_pdu_hdr_t *hdr_out)
{
    if (!buf || !hdr_out || buf_len < LDP_PDU_HEADER_SIZE)
    {
        return -1;
    }

    hdr_out->version = get_u16(buf);
    hdr_out->pdu_length = get_u16(buf + 2);
    hdr_out->lsr_id = get_u32(buf + 4);
    hdr_out->label_space = get_u16(buf + 8);

    if (hdr_out->version != LDP_VERSION)
    {
        return -1;
    }
    if (hdr_out->pdu_length < 14u || (size_t)hdr_out->pdu_length + 4u > buf_len)
    {
        return -1;
    }
    return 0;
}

int ldp_pkt_parse_msg_hdr(const uint8_t *buf, size_t buf_len, ldp_msg_hdr_t *hdr_out)
{
    if (!buf || !hdr_out || buf_len < LDP_MSG_HEADER_SIZE)
    {
        return -1;
    }

    uint16_t type_field = get_u16(buf);
    hdr_out->u_bit = (type_field & 0x8000u) ? 1u : 0u;
    hdr_out->msg_type = (uint16_t)(type_field & 0x7FFFu);
    hdr_out->msg_length = get_u16(buf + 2);
    hdr_out->msg_id = get_u32(buf + 4);

    if (hdr_out->msg_length < 4u || (size_t)hdr_out->msg_length + 4u > buf_len)
    {
        return -1;
    }
    return 0;
}

int ldp_pkt_parse_hello(const uint8_t *body, size_t body_len, ldp_hello_info_t *info_out)
{
    if (!body || !info_out)
    {
        return -1;
    }

    memset(info_out, 0, sizeof(*info_out));

    size_t pos = 0;
    int got_common = 0;
    while (pos + 4u <= body_len)
    {
        uint16_t tlv_type = get_u16(body + pos);
        uint16_t tlv_len = get_u16(body + pos + 2);
        /* TLV type 高 2 位是 U/F bits，按规范 mask 掉 */
        uint16_t tlv_type_masked = (uint16_t)(tlv_type & 0x3FFFu);
        pos += 4u;
        if (pos + (size_t)tlv_len > body_len)
        {
            return -1;
        }

        const uint8_t *val = body + pos;
        switch (tlv_type_masked)
        {
            case LDP_TLV_COMMON_HELLO_PARAMS:
                if (tlv_len >= 4u)
                {
                    info_out->hold_time_sec = get_u16(val);
                    uint16_t flags = get_u16(val + 2);
                    info_out->targeted = (flags & 0x8000u) ? 1u : 0u;
                    info_out->request_targeted = (flags & 0x4000u) ? 1u : 0u;
                    got_common = 1;
                }
                break;
            case LDP_TLV_IPV4_TRANSPORT_ADDR:
                if (tlv_len >= 4u)
                {
                    info_out->transport_v4 = get_u32(val);
                }
                break;
            case LDP_TLV_CONFIG_SEQ_NUMBER:
                if (tlv_len >= 4u)
                {
                    info_out->configuration_seq = get_u32(val);
                }
                break;
            default:
                break;
        }
        pos += tlv_len;
    }

    if (!got_common)
    {
        return -1;
    }
    info_out->valid = 1u;
    return 0;
}

int ldp_pkt_encode_init(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint16_t keepalive_sec,
                        uint32_t recv_lsr_id, uint16_t recv_label_space, uint8_t *out, size_t out_cap)
{
    if (!out)
    {
        return -1;
    }
    /* TLV value 大小 = 14 bytes：
     *   protocol_version(2) + keepalive_time(2) + flags(2) + pvlim+max_pdu(? 2+2 = 4 actually)
     * 实际格式（RFC 5036 §3.5.3）：
     *   PV(2) | KAT(2) | A|D|RES(2) | PVLim(1) | _(1) | MaxPDULen(2) | RxLDPId(LSR4+space2)
     */
    const size_t tlv_value_len = 14u;
    const size_t msg_body_len = 4u + tlv_value_len; /* TLV header(4) + value */
    const size_t msg_total = LDP_MSG_HEADER_SIZE + msg_body_len;
    const size_t pdu_total = LDP_PDU_HEADER_SIZE + msg_total;

    if (out_cap < pdu_total)
    {
        return -1;
    }

    uint8_t *p = out;
    /* PDU header */
    put_u16(p, LDP_VERSION);
    p += 2;
    put_u16(p, (uint16_t)(6u + msg_total));
    p += 2;
    put_u32(p, self_lsr_id);
    p += 4;
    put_u16(p, self_label_space);
    p += 2;

    /* Msg header */
    put_u16(p, LDP_MSG_TYPE_INITIALIZATION);
    p += 2;
    put_u16(p, (uint16_t)(4u + msg_body_len));
    p += 2;
    put_u32(p, msg_id);
    p += 4;

    /* Common Session Parameters TLV (RFC 5036 §3.5.3)
     *   Protocol Version       (2)
     *   KeepAlive Time         (2)
     *   A|D|Reserved           (1)   ← A=高位 bit，D=次高 bit，其余 6 bit reserved
     *   PVLim                  (1)
     *   Max PDU Length         (2)
     *   Receiver LDP Identifier (6)  ← LSR-ID(4) + Label Space(2)
     * 合计 14 字节，与 TLV 头 length=14 对齐。
     * 注意：A/D/Reserved 是 1 字节，不是 2 字节；FRR/OpenBSD ldpd 都这么编。
     */
    put_u16(p, LDP_TLV_COMMON_SESSION_PARAMS);
    p += 2;
    put_u16(p, (uint16_t)tlv_value_len);
    p += 2;
    put_u16(p, LDP_VERSION);
    p += 2;
    put_u16(p, keepalive_sec);
    p += 2;
    *p++ = 0u;      /* A=0 D=0 Reserved */
    *p++ = 0u;      /* PVLim */
    put_u16(p, 0u); /* Max PDU Length = default */
    p += 2;
    put_u32(p, recv_lsr_id);
    p += 4;
    put_u16(p, recv_label_space);
    p += 2;

    return (int)(p - out);
}

int ldp_pkt_encode_keepalive(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint8_t *out,
                             size_t out_cap)
{
    if (!out)
    {
        return -1;
    }
    const size_t msg_total = LDP_MSG_HEADER_SIZE; /* body 为空 */
    const size_t pdu_total = LDP_PDU_HEADER_SIZE + msg_total;
    if (out_cap < pdu_total)
    {
        return -1;
    }

    uint8_t *p = out;
    put_u16(p, LDP_VERSION);
    p += 2;
    put_u16(p, (uint16_t)(6u + msg_total));
    p += 2;
    put_u32(p, self_lsr_id);
    p += 4;
    put_u16(p, self_label_space);
    p += 2;

    put_u16(p, LDP_MSG_TYPE_KEEPALIVE);
    p += 2;
    put_u16(p, 4u); /* length 字段含 msg_id 自身 */
    p += 2;
    put_u32(p, msg_id);
    p += 4;
    return (int)(p - out);
}

int ldp_pkt_encode_notification(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint32_t status_code,
                                int fatal, uint32_t ref_msg_id, uint16_t ref_msg_type, uint8_t *out, size_t out_cap)
{
    if (!out)
    {
        return -1;
    }
    const size_t status_tlv_value_len = 10u; /* Status Code(4) + Msg ID(4) + Msg Type(2) */
    const size_t status_tlv_total = 4u + status_tlv_value_len;
    const size_t msg_body_len = status_tlv_total;
    const size_t msg_total = LDP_MSG_HEADER_SIZE + msg_body_len;
    const size_t pdu_total = LDP_PDU_HEADER_SIZE + msg_total;
    if (out_cap < pdu_total)
    {
        return -1;
    }

    uint8_t *p = out;
    put_u16(p, LDP_VERSION);
    p += 2;
    put_u16(p, (uint16_t)(6u + msg_total));
    p += 2;
    put_u32(p, self_lsr_id);
    p += 4;
    put_u16(p, self_label_space);
    p += 2;

    put_u16(p, LDP_MSG_TYPE_NOTIFICATION);
    p += 2;
    put_u16(p, (uint16_t)(4u + msg_body_len));
    p += 2;
    put_u32(p, msg_id);
    p += 4;

    put_u16(p, LDP_TLV_STATUS);
    p += 2;
    put_u16(p, (uint16_t)status_tlv_value_len);
    p += 2;
    put_u32(p, (status_code & 0x3FFFFFFFu) | (fatal ? LDP_STATUS_FATAL_BIT : 0u));
    p += 4;
    put_u32(p, ref_msg_id);
    p += 4;
    put_u16(p, ref_msg_type);
    p += 2;
    return (int)(p - out);
}

int ldp_pkt_parse_init(const uint8_t *body, size_t body_len, ldp_init_info_t *info_out)
{
    if (!body || !info_out)
    {
        return -1;
    }
    memset(info_out, 0, sizeof(*info_out));

    size_t pos = 0;
    while (pos + 4u <= body_len)
    {
        uint16_t tlv_type = get_u16(body + pos);
        uint16_t tlv_len = get_u16(body + pos + 2);
        uint16_t tlv_type_masked = (uint16_t)(tlv_type & 0x3FFFu);
        pos += 4u;
        if (pos + (size_t)tlv_len > body_len)
        {
            return -1;
        }

        if (tlv_type_masked == LDP_TLV_COMMON_SESSION_PARAMS)
        {
            if (tlv_len < 14u)
            {
                return -1;
            }
            const uint8_t *v = body + pos;
            info_out->protocol_version = get_u16(v);
            info_out->keepalive_time_sec = get_u16(v + 2);
            info_out->a_bit = (v[4] & 0x80u) ? 1u : 0u;
            info_out->d_bit = (v[4] & 0x40u) ? 1u : 0u;
            info_out->pvlim = (uint16_t)v[5];
            info_out->max_pdu_len = get_u16(v + 6);
            info_out->recv_lsr_id = get_u32(v + 8);
            info_out->recv_label_space = get_u16(v + 12);
            info_out->valid = 1u;
        }
        pos += tlv_len;
    }

    return info_out->valid ? 0 : -1;
}

// ============================================================================
// Address / Address Withdraw
// ============================================================================

int ldp_pkt_encode_address(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, int withdraw,
                           const uint32_t *addrs, size_t n_addrs, uint8_t *out, size_t out_cap)
{
    if (!out || (n_addrs > 0 && !addrs))
    {
        return -1;
    }
    /* Address List TLV value = AF(2) + 4*n */
    const size_t tlv_value_len = 2u + 4u * n_addrs;
    const size_t msg_body_len = 4u + tlv_value_len; /* TLV header(4) + value */
    const size_t msg_total = LDP_MSG_HEADER_SIZE + msg_body_len;
    const size_t pdu_total = LDP_PDU_HEADER_SIZE + msg_total;
    if (out_cap < pdu_total)
    {
        return -1;
    }

    uint8_t *p = out;
    put_u16(p, LDP_VERSION);
    p += 2;
    put_u16(p, (uint16_t)(6u + msg_total));
    p += 2;
    put_u32(p, self_lsr_id);
    p += 4;
    put_u16(p, self_label_space);
    p += 2;

    put_u16(p, withdraw ? LDP_MSG_TYPE_ADDRESS_WITHDRAW : LDP_MSG_TYPE_ADDRESS);
    p += 2;
    put_u16(p, (uint16_t)(4u + msg_body_len));
    p += 2;
    put_u32(p, msg_id);
    p += 4;

    put_u16(p, LDP_TLV_ADDRESS_LIST);
    p += 2;
    put_u16(p, (uint16_t)tlv_value_len);
    p += 2;
    put_u16(p, LDP_AFNUM_IPV4);
    p += 2;
    for (size_t i = 0; i < n_addrs; i++)
    {
        put_u32(p, addrs[i]);
        p += 4;
    }
    return (int)(p - out);
}

int ldp_pkt_parse_address_list_tlv(const uint8_t *body, size_t body_len, uint32_t *addrs_out, size_t cap)
{
    if (!body)
    {
        return -1;
    }
    size_t pos = 0;
    while (pos + 4u <= body_len)
    {
        uint16_t tlv_type = get_u16(body + pos);
        uint16_t tlv_len = get_u16(body + pos + 2);
        uint16_t tlv_type_masked = (uint16_t)(tlv_type & 0x3FFFu);
        pos += 4u;
        if (pos + (size_t)tlv_len > body_len)
        {
            return -1;
        }
        if (tlv_type_masked == LDP_TLV_ADDRESS_LIST)
        {
            if (tlv_len < 2u)
            {
                return -1;
            }
            uint16_t af = get_u16(body + pos);
            if (af != LDP_AFNUM_IPV4)
            {
                pos += tlv_len;
                continue;
            }
            size_t n = (tlv_len - 2u) / 4u;
            size_t k = 0;
            for (size_t i = 0; i < n && k < cap; i++)
            {
                if (addrs_out)
                {
                    addrs_out[k] = get_u32(body + pos + 2u + 4u * i);
                }
                k++;
            }
            return (int)k;
        }
        pos += tlv_len;
    }
    return 0;
}

// ============================================================================
// Label Mapping / Withdraw
// ============================================================================

/*
 * IPv4 Prefix FEC element:
 *   [type=2:1][address_family:2][prefix_len:1][prefix(...)]
 * 其中 prefix 字节数 = ceil(prefix_len/8)
 *
 * Generic Label TLV value = 4 bytes label value.
 */

static size_t prefix_octets(uint8_t plen)
{
    return ((size_t)plen + 7u) / 8u;
}

static int encode_fec_tlv(uint8_t *p, uint32_t prefix, uint8_t prefix_len)
{
    size_t octets = prefix_octets(prefix_len);
    size_t fec_elem_size = 1u + 2u + 1u + octets;
    size_t tlv_total = 4u + fec_elem_size;
    /* p must have at least tlv_total bytes */
    put_u16(p, LDP_TLV_FEC);
    put_u16(p + 2, (uint16_t)fec_elem_size);
    p += 4;
    *p++ = LDP_FEC_ELEM_PREFIX;
    put_u16(p, LDP_AFNUM_IPV4);
    p += 2;
    *p++ = prefix_len;
    /* prefix in network byte order, truncated */
    uint32_t v = prefix;
    uint8_t bytes[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    for (size_t i = 0; i < octets; i++)
    {
        *p++ = bytes[i];
    }
    return (int)tlv_total;
}

static int encode_generic_label_tlv(uint8_t *p, uint32_t label)
{
    put_u16(p, LDP_TLV_GENERIC_LABEL);
    put_u16(p + 2, 4u);
    put_u32(p + 4, label & 0x000FFFFFu);
    return 8;
}

int ldp_pkt_encode_label_mapping(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint32_t fec_prefix,
                                 uint8_t fec_prefix_len, uint32_t label, uint8_t *out, size_t out_cap)
{
    if (!out || fec_prefix_len > 32)
    {
        return -1;
    }
    size_t octets = prefix_octets(fec_prefix_len);
    /* fec_tlv_total 已含 FEC TLV header(4)；label_tlv_total 同理。msg body 不需要再
     * 额外 +4（之前误把 TLV header 重复加了一次，导致 pdu_length 比实际多 4，
     * 接收侧多吃 4 字节，后续 PDU 全部错位）。*/
    size_t fec_tlv_total = 4u + 1u + 2u + 1u + octets;
    size_t label_tlv_total = 8u;
    size_t msg_body_len = fec_tlv_total + label_tlv_total;
    size_t msg_total = LDP_MSG_HEADER_SIZE + msg_body_len;
    size_t pdu_total = LDP_PDU_HEADER_SIZE + msg_total;
    if (out_cap < pdu_total)
    {
        return -1;
    }

    uint8_t *p = out;
    put_u16(p, LDP_VERSION);
    p += 2;
    put_u16(p, (uint16_t)(6u + msg_total));
    p += 2;
    put_u32(p, self_lsr_id);
    p += 4;
    put_u16(p, self_label_space);
    p += 2;

    put_u16(p, LDP_MSG_TYPE_LABEL_MAPPING);
    p += 2;
    /* msg_length = msg_id(4) + body */
    put_u16(p, (uint16_t)(4u + msg_body_len));
    p += 2;
    put_u32(p, msg_id);
    p += 4;

    p += encode_fec_tlv(p, fec_prefix, fec_prefix_len);
    p += encode_generic_label_tlv(p, label);
    return (int)(p - out);
}

int ldp_pkt_encode_label_withdraw(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint32_t fec_prefix,
                                  uint8_t fec_prefix_len, uint8_t *out, size_t out_cap)
{
    if (!out || fec_prefix_len > 32)
    {
        return -1;
    }
    size_t octets = prefix_octets(fec_prefix_len);
    /* fec_tlv_total 已含 TLV header；msg body 不要再额外 +4 */
    size_t fec_tlv_total = 4u + 1u + 2u + 1u + octets;
    size_t msg_body_len = fec_tlv_total;
    size_t msg_total = LDP_MSG_HEADER_SIZE + msg_body_len;
    size_t pdu_total = LDP_PDU_HEADER_SIZE + msg_total;
    if (out_cap < pdu_total)
    {
        return -1;
    }

    uint8_t *p = out;
    put_u16(p, LDP_VERSION);
    p += 2;
    put_u16(p, (uint16_t)(6u + msg_total));
    p += 2;
    put_u32(p, self_lsr_id);
    p += 4;
    put_u16(p, self_label_space);
    p += 2;

    put_u16(p, LDP_MSG_TYPE_LABEL_WITHDRAW);
    p += 2;
    put_u16(p, (uint16_t)(4u + msg_body_len));
    p += 2;
    put_u32(p, msg_id);
    p += 4;

    p += encode_fec_tlv(p, fec_prefix, fec_prefix_len);
    return (int)(p - out);
}

int ldp_pkt_encode_label_release(uint32_t self_lsr_id, uint16_t self_label_space, uint32_t msg_id, uint32_t fec_prefix,
                                 uint8_t fec_prefix_len, uint32_t label, int include_label, uint8_t *out,
                                 size_t out_cap)
{
    if (!out || fec_prefix_len > 32)
    {
        return -1;
    }
    size_t octets = prefix_octets(fec_prefix_len);
    size_t fec_tlv_total = 4u + 1u + 2u + 1u + octets;
    size_t label_tlv_total = include_label ? 8u : 0u;
    size_t msg_body_len = fec_tlv_total + label_tlv_total;
    size_t msg_total = LDP_MSG_HEADER_SIZE + msg_body_len;
    size_t pdu_total = LDP_PDU_HEADER_SIZE + msg_total;
    if (out_cap < pdu_total)
    {
        return -1;
    }

    uint8_t *p = out;
    put_u16(p, LDP_VERSION);
    p += 2;
    put_u16(p, (uint16_t)(6u + msg_total));
    p += 2;
    put_u32(p, self_lsr_id);
    p += 4;
    put_u16(p, self_label_space);
    p += 2;

    put_u16(p, LDP_MSG_TYPE_LABEL_RELEASE);
    p += 2;
    put_u16(p, (uint16_t)(4u + msg_body_len));
    p += 2;
    put_u32(p, msg_id);
    p += 4;

    p += encode_fec_tlv(p, fec_prefix, fec_prefix_len);
    if (include_label)
    {
        p += encode_generic_label_tlv(p, label);
    }
    return (int)(p - out);
}

static int parse_fec_tlv(const uint8_t *body, size_t body_len, uint32_t *prefix_out, uint8_t *plen_out)
{
    if (!body || body_len < 4u)
    {
        return -1;
    }
    uint16_t tlv_type = get_u16(body);
    uint16_t tlv_len = get_u16(body + 2);
    if ((uint16_t)(tlv_type & 0x3FFFu) != LDP_TLV_FEC)
    {
        return -1;
    }
    if ((size_t)tlv_len + 4u > body_len)
    {
        return -1;
    }

    /* 解析第一个 FEC element（M4 仅识别 IPv4 Prefix） */
    if (tlv_len < 1u + 2u + 1u)
    {
        return -1;
    }
    const uint8_t *v = body + 4;
    uint8_t fec_type = v[0];
    if (fec_type != LDP_FEC_ELEM_PREFIX)
    {
        /* 跳过整个 FEC TLV，但 M4 当成失败 */
        return -1;
    }
    uint16_t af = get_u16(v + 1);
    uint8_t plen = v[3];
    if (af != LDP_AFNUM_IPV4 || plen > 32u)
    {
        return -1;
    }
    size_t octets = ((size_t)plen + 7u) / 8u;
    if (1u + 2u + 1u + octets > tlv_len)
    {
        return -1;
    }
    uint32_t pref = 0u;
    for (size_t i = 0; i < octets; i++)
    {
        pref |= (uint32_t)v[4 + i] << (24 - 8 * i);
    }
    /* 清理未占用低位 */
    if (plen < 32u)
    {
        uint32_t mask = (plen == 0u) ? 0u : (uint32_t)(0xFFFFFFFFu << (32u - plen));
        pref &= mask;
    }
    *prefix_out = pref;
    *plen_out = plen;
    return (int)(4u + tlv_len);
}

static int parse_generic_label_tlv(const uint8_t *body, size_t body_len, uint32_t *label_out)
{
    if (!body || body_len < 4u)
    {
        return -1;
    }
    uint16_t tlv_type = get_u16(body);
    uint16_t tlv_len = get_u16(body + 2);
    if ((uint16_t)(tlv_type & 0x3FFFu) != LDP_TLV_GENERIC_LABEL)
    {
        return -1;
    }
    if ((size_t)tlv_len + 4u > body_len || tlv_len < 4u)
    {
        return -1;
    }
    *label_out = get_u32(body + 4) & 0x000FFFFFu;
    return (int)(4u + tlv_len);
}

int ldp_pkt_parse_label_msg(const uint8_t *body, size_t body_len, uint32_t *fec_prefix_out, uint8_t *fec_prefix_len_out,
                            uint32_t *label_out)
{
    if (!body || !fec_prefix_out || !fec_prefix_len_out || !label_out)
    {
        return -1;
    }
    *label_out = 0u;
    int n = parse_fec_tlv(body, body_len, fec_prefix_out, fec_prefix_len_out);
    if (n < 0)
    {
        return -1;
    }
    /* 后续可能跟 Label TLV（Mapping）或没有（Withdraw） */
    size_t pos = (size_t)n;
    if (pos + 4u <= body_len)
    {
        uint16_t next_type = (uint16_t)(get_u16(body + pos) & 0x3FFFu);
        if (next_type == LDP_TLV_GENERIC_LABEL)
        {
            (void)parse_generic_label_tlv(body + pos, body_len - pos, label_out);
        }
    }
    return 0;
}
