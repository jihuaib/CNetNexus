/**
 * @file   bgp_prefix_sid.c
 * @brief  BGP Prefix-SID / RFC 9252 SRv6 L3 Service 属性解析与构造
 */
#include <string.h>
#include <sys/socket.h>

#include "bgp_parse_priv.h"

#define PREFIX_SID_FLAG_OPTIONAL 0x80u
#define PREFIX_SID_FLAG_TRANSITIVE 0x40u

#define SRV6_SID_INFO_FIXED_LEN 21u
#define SRV6_SID_STRUCTURE_LEN 6u

/* RFC 8986 SRv6 Endpoint Behaviors registry. */
#define SRV6_BEHAVIOR_END_DT6 18u
#define SRV6_BEHAVIOR_END_DT4 19u

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

void bgp_attr_clear_prefix_sid(bgp_attr_t *attr)
{
    if (!attr)
    {
        return;
    }

    attr->has_prefix_sid = false;
    attr->prefix_sid_attr_flags = 0;
    attr->prefix_sid_raw_len = 0;
    memset(attr->prefix_sid_raw, 0, sizeof(attr->prefix_sid_raw));

    attr->has_srv6_l3_service_tlv = false;
    attr->has_srv6_l3_service = false;
    memset(&attr->srv6_sid, 0, sizeof(attr->srv6_sid));
    attr->srv6_behavior = 0;
    attr->srv6_sid_flags = 0;

    attr->has_srv6_sid_structure = false;
    attr->locator_block_len = 0;
    attr->locator_node_len = 0;
    attr->function_len = 0;
    attr->argument_len = 0;
    attr->transposition_len = 0;
    attr->transposition_offset = 0;
}

static bool sid_bits_are_zero(const uint8_t sid[16], uint8_t offset, uint8_t len)
{
    if ((uint16_t)offset + len > 128u)
    {
        return false;
    }
    for (uint16_t bit = offset; bit < (uint16_t)offset + len; bit++)
    {
        uint8_t mask = (uint8_t)(0x80u >> (bit & 7u));
        if ((sid[bit >> 3] & mask) != 0)
        {
            return false;
        }
    }
    return true;
}

static bool sid_structure_valid(const bgp_srv6_sid_structure_t *structure, const uint8_t sid[16], uint16_t behavior,
                                bool require_whole_sid)
{
    if (!structure)
    {
        return true;
    }

    uint16_t sid_parts_len = (uint16_t)structure->locator_block_len + structure->locator_node_len +
                             structure->function_len + structure->argument_len;
    uint16_t transposed_end = (uint16_t)structure->transposition_offset + structure->transposition_len;

    /* RFC 9252 verified erratum 7268 permits equality here. */
    if (sid_parts_len == 0u || sid_parts_len > 128u || sid_parts_len < transposed_end)
    {
        return false;
    }
    if (structure->transposition_len == 0u && structure->transposition_offset != 0u)
    {
        return false;
    }
    if (require_whole_sid && structure->transposition_len != 0u)
    {
        return false;
    }
    /* 24 is the largest service label field covered by RFC 9252.  The
     * consuming AF performs the stricter VPN label (20-bit) check. */
    if (structure->transposition_len > 24u)
    {
        return false;
    }
    if (structure->transposition_len > structure->function_len)
    {
        return false;
    }
    if (structure->transposition_len > 0u &&
        !sid_bits_are_zero(sid, structure->transposition_offset, structure->transposition_len))
    {
        return false;
    }

    /* End.DT4/End.DT6 do not define SID arguments. */
    if ((behavior == SRV6_BEHAVIOR_END_DT4 || behavior == SRV6_BEHAVIOR_END_DT6) && structure->argument_len != 0u)
    {
        return false;
    }
    /* For endpoint behaviors not understood here, RFC 9252 requires a SID
     * carrying arguments to be ignored rather than guessed. */
    if (behavior != SRV6_BEHAVIOR_END_DT4 && behavior != SRV6_BEHAVIOR_END_DT6 && structure->argument_len != 0u)
    {
        return false;
    }

    return true;
}

typedef struct parsed_sid_info
{
    uint8_t sid[16];
    uint8_t flags;
    uint16_t behavior;
    bool valid;
    bool has_structure;
    bgp_srv6_sid_structure_t structure;
} parsed_sid_info_t;

/**
 * Validate one SID Information Sub-TLV.  Container length errors are
 * malformed (return -1); invalid SID Structure values merely make this SID
 * ineligible while leaving the optional-transitive attribute propagatable.
 */
static int parse_sid_info(const uint8_t *value, uint16_t len, bool capture, parsed_sid_info_t *out)
{
    if (len < SRV6_SID_INFO_FIXED_LEN)
    {
        return -1;
    }

    parsed_sid_info_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    memcpy(candidate.sid, value + 1, sizeof(candidate.sid)); /* RESERVED1 precedes SID */
    candidate.flags = value[17];
    candidate.behavior = read_u16(value + 18);
    candidate.valid = true;

    uint16_t pos = SRV6_SID_INFO_FIXED_LEN;
    bool structure_seen = false;
    while (pos < len)
    {
        if ((uint16_t)(len - pos) < 3u)
        {
            return -1;
        }
        uint8_t type = value[pos];
        uint16_t sub_len = read_u16(value + pos + 1);
        pos = (uint16_t)(pos + 3u);
        if (sub_len > (uint16_t)(len - pos))
        {
            return -1;
        }

        if (type == BGP_SRV6_SERVICE_DATA_SUBSUBTLV_SID_STRUCTURE)
        {
            if (!structure_seen)
            {
                structure_seen = true;
                if (sub_len != SRV6_SID_STRUCTURE_LEN)
                {
                    candidate.valid = false;
                }
                else
                {
                    candidate.has_structure = true;
                    candidate.structure.locator_block_len = value[pos];
                    candidate.structure.locator_node_len = value[pos + 1];
                    candidate.structure.function_len = value[pos + 2];
                    candidate.structure.argument_len = value[pos + 3];
                    candidate.structure.transposition_len = value[pos + 4];
                    candidate.structure.transposition_offset = value[pos + 5];
                    if (!sid_structure_valid(&candidate.structure, candidate.sid, candidate.behavior, false))
                    {
                        candidate.valid = false;
                    }
                }
            }
        }
        pos = (uint16_t)(pos + sub_len);
    }

    if (capture && out)
    {
        *out = candidate;
    }
    return 0;
}

static int parse_l3_service_tlv(const uint8_t *value, uint16_t len, bool capture, parsed_sid_info_t *out)
{
    if (len < 1u) /* leading RESERVED octet */
    {
        return -1;
    }

    uint16_t pos = 1u;
    bool sid_info_seen = false;
    while (pos < len)
    {
        if ((uint16_t)(len - pos) < 3u)
        {
            return -1;
        }
        uint8_t type = value[pos];
        uint16_t sub_len = read_u16(value + pos + 1);
        pos = (uint16_t)(pos + 3u);
        if (sub_len > (uint16_t)(len - pos))
        {
            return -1;
        }

        if (type == BGP_SRV6_SERVICE_SUBTLV_SID_INFO)
        {
            bool capture_this = capture && !sid_info_seen;
            if (parse_sid_info(value + pos, sub_len, capture_this, out) != 0)
            {
                return -1;
            }
            sid_info_seen = true;
        }
        pos = (uint16_t)(pos + sub_len);
    }
    return 0;
}

int bgp_parse_prefix_sid_value(const uint8_t *data, uint16_t len, uint8_t attr_flags, bgp_attr_t *attr)
{
    if (!attr || (len > 0u && !data) || len > BGP_ATTR_PREFIX_SID_MAX)
    {
        return BGP_PREFIX_SID_PARSE_ATTRIBUTE_DISCARD;
    }

    bgp_attr_clear_prefix_sid(attr);
    attr->has_prefix_sid = true;
    attr->prefix_sid_attr_flags = attr_flags;
    attr->prefix_sid_raw_len = len;
    if (len > 0u)
    {
        memcpy(attr->prefix_sid_raw, data, len);
    }

    parsed_sid_info_t first_sid;
    memset(&first_sid, 0, sizeof(first_sid));
    bool l3_service_seen = false;
    uint16_t pos = 0;
    while (pos < len)
    {
        if ((uint16_t)(len - pos) < 3u)
        {
            bgp_attr_clear_prefix_sid(attr);
            return BGP_PREFIX_SID_PARSE_ATTRIBUTE_DISCARD;
        }
        uint8_t type = data[pos];
        uint16_t tlv_len = read_u16(data + pos + 1);
        pos = (uint16_t)(pos + 3u);
        if (tlv_len > (uint16_t)(len - pos))
        {
            bgp_prefix_sid_parse_result_t rc = (type == BGP_PREFIX_SID_TLV_SRV6_L3_SERVICE)
                                                   ? BGP_PREFIX_SID_PARSE_TREAT_AS_WITHDRAW
                                                   : BGP_PREFIX_SID_PARSE_ATTRIBUTE_DISCARD;
            if (rc == BGP_PREFIX_SID_PARSE_ATTRIBUTE_DISCARD)
            {
                bgp_attr_clear_prefix_sid(attr);
            }
            return rc;
        }

        if (type == BGP_PREFIX_SID_TLV_SRV6_L3_SERVICE)
        {
            attr->has_srv6_l3_service_tlv = true;
            /* RFC 9252: 多个 L3 Service TLV 只处理第一个；后续 TLV 的
             * 内部内容必须整体忽略，不得使属性 treat-as-withdraw。 */
            if (!l3_service_seen && parse_l3_service_tlv(data + pos, tlv_len, true, &first_sid) != 0)
            {
                return BGP_PREFIX_SID_PARSE_TREAT_AS_WITHDRAW;
            }
            l3_service_seen = true;
        }
        pos = (uint16_t)(pos + tlv_len);
    }

    if (l3_service_seen && first_sid.valid)
    {
        attr->has_srv6_l3_service = true;
        attr->srv6_sid.family = AF_INET6;
        memcpy(attr->srv6_sid.u.v6.s6_addr, first_sid.sid, sizeof(first_sid.sid));
        attr->srv6_behavior = first_sid.behavior;
        attr->srv6_sid_flags = first_sid.flags;
        if (first_sid.has_structure)
        {
            attr->has_srv6_sid_structure = true;
            attr->locator_block_len = first_sid.structure.locator_block_len;
            attr->locator_node_len = first_sid.structure.locator_node_len;
            attr->function_len = first_sid.structure.function_len;
            attr->argument_len = first_sid.structure.argument_len;
            attr->transposition_len = first_sid.structure.transposition_len;
            attr->transposition_offset = first_sid.structure.transposition_offset;
        }
    }
    return BGP_PREFIX_SID_PARSE_OK;
}

int bgp_attr_set_srv6_l3_service(bgp_attr_t *attr, const net_addr_t *sid, uint16_t behavior, uint8_t sid_flags,
                                 const bgp_srv6_sid_structure_t *structure)
{
    if (!attr || !sid || sid->family != AF_INET6 ||
        (structure && !sid_structure_valid(structure, sid->u.v6.s6_addr, behavior, true)))
    {
        return -1;
    }

    uint16_t structure_total = structure ? (uint16_t)(3u + SRV6_SID_STRUCTURE_LEN) : 0u;
    uint16_t sid_info_value_len = (uint16_t)(SRV6_SID_INFO_FIXED_LEN + structure_total);
    uint16_t sid_info_total = (uint16_t)(3u + sid_info_value_len);
    uint16_t l3_value_len = (uint16_t)(1u + sid_info_total);
    uint16_t raw_len = (uint16_t)(3u + l3_value_len);
    if (raw_len > BGP_ATTR_PREFIX_SID_MAX)
    {
        return -1;
    }

    bgp_attr_clear_prefix_sid(attr);
    uint8_t *raw = attr->prefix_sid_raw;
    uint16_t pos = 0;
    raw[pos++] = BGP_PREFIX_SID_TLV_SRV6_L3_SERVICE;
    write_u16(raw + pos, l3_value_len);
    pos = (uint16_t)(pos + 2u);
    raw[pos++] = 0; /* L3 Service TLV RESERVED */

    raw[pos++] = BGP_SRV6_SERVICE_SUBTLV_SID_INFO;
    write_u16(raw + pos, sid_info_value_len);
    pos = (uint16_t)(pos + 2u);
    raw[pos++] = 0; /* SID Information RESERVED1 */
    memcpy(raw + pos, sid->u.v6.s6_addr, 16);
    pos = (uint16_t)(pos + 16u);
    raw[pos++] = sid_flags;
    write_u16(raw + pos, behavior);
    pos = (uint16_t)(pos + 2u);
    raw[pos++] = 0; /* SID Information RESERVED2 */

    if (structure)
    {
        raw[pos++] = BGP_SRV6_SERVICE_DATA_SUBSUBTLV_SID_STRUCTURE;
        write_u16(raw + pos, SRV6_SID_STRUCTURE_LEN);
        pos = (uint16_t)(pos + 2u);
        raw[pos++] = structure->locator_block_len;
        raw[pos++] = structure->locator_node_len;
        raw[pos++] = structure->function_len;
        raw[pos++] = structure->argument_len;
        raw[pos++] = structure->transposition_len;
        raw[pos++] = structure->transposition_offset;
    }

    if (pos != raw_len)
    {
        bgp_attr_clear_prefix_sid(attr);
        return -1;
    }

    attr->has_prefix_sid = true;
    attr->prefix_sid_attr_flags = PREFIX_SID_FLAG_OPTIONAL | PREFIX_SID_FLAG_TRANSITIVE;
    attr->prefix_sid_raw_len = raw_len;
    attr->has_srv6_l3_service_tlv = true;
    attr->has_srv6_l3_service = true;
    attr->srv6_sid = *sid;
    attr->srv6_behavior = behavior;
    attr->srv6_sid_flags = sid_flags;
    if (structure)
    {
        attr->has_srv6_sid_structure = true;
        attr->locator_block_len = structure->locator_block_len;
        attr->locator_node_len = structure->locator_node_len;
        attr->function_len = structure->function_len;
        attr->argument_len = structure->argument_len;
        attr->transposition_len = structure->transposition_len;
        attr->transposition_offset = structure->transposition_offset;
    }
    return 0;
}
