/**
 * @file   lldp_packet.c
 * @brief  LLDP TLV 编解码
 * @author jhb
 * @date   2026/06/07
 */
#include "lldp_packet.h"

#include <string.h>

#include "errcode.h"

static int put_u16(uint8_t *buf, size_t cap, size_t *pos, uint16_t v)
{
    if (!buf || !pos || *pos + 2u > cap)
    {
        return ERRCODE_FAIL;
    }
    buf[(*pos)++] = (uint8_t)((v >> 8) & 0xFFu);
    buf[(*pos)++] = (uint8_t)(v & 0xFFu);
    return ERRCODE_SUCCESS;
}

static int put_u32(uint8_t *buf, size_t cap, size_t *pos, uint32_t v)
{
    if (!buf || !pos || *pos + 4u > cap)
    {
        return ERRCODE_FAIL;
    }
    buf[(*pos)++] = (uint8_t)((v >> 24) & 0xFFu);
    buf[(*pos)++] = (uint8_t)((v >> 16) & 0xFFu);
    buf[(*pos)++] = (uint8_t)((v >> 8) & 0xFFu);
    buf[(*pos)++] = (uint8_t)(v & 0xFFu);
    return ERRCODE_SUCCESS;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int append_tlv(uint8_t *buf, size_t cap, size_t *pos, uint8_t type, const uint8_t *value, size_t value_len)
{
    if (!buf || !pos || value_len > LLDP_TLV_MAX_VALUE_LEN || *pos + 2u + value_len > cap)
    {
        return ERRCODE_FAIL;
    }
    uint16_t header = (uint16_t)(((uint16_t)(type & 0x7Fu) << 9) | (uint16_t)value_len);
    if (put_u16(buf, cap, pos, header) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (value_len > 0u)
    {
        if (!value)
        {
            return ERRCODE_FAIL;
        }
        memcpy(buf + *pos, value, value_len);
        *pos += value_len;
    }
    return ERRCODE_SUCCESS;
}

static int append_id_tlv(uint8_t *buf, size_t cap, size_t *pos, uint8_t type, const lldp_id_t *id)
{
    if (!id || id->subtype == 0u || id->len == 0u || id->len > LLDP_ID_MAX_LEN)
    {
        return ERRCODE_FAIL;
    }

    uint8_t value[1u + LLDP_ID_MAX_LEN];
    value[0] = id->subtype;
    memcpy(value + 1u, id->data, id->len);
    return append_tlv(buf, cap, pos, type, value, (size_t)id->len + 1u);
}

static size_t bounded_strlen(const char *s, size_t max_len)
{
    if (!s)
    {
        return 0u;
    }
    size_t len = 0u;
    while (len < max_len && s[len] != '\0')
    {
        len++;
    }
    return len;
}

static int append_string_tlv(uint8_t *buf, size_t cap, size_t *pos, uint8_t type, const char *s)
{
    size_t len = bounded_strlen(s, LLDP_TLV_MAX_VALUE_LEN);
    if (len == 0u)
    {
        return ERRCODE_SUCCESS;
    }
    return append_tlv(buf, cap, pos, type, (const uint8_t *)s, len);
}

static int append_ttl_tlv(uint8_t *buf, size_t cap, size_t *pos, uint16_t ttl)
{
    uint8_t value[2] = {
        (uint8_t)((ttl >> 8) & 0xFFu),
        (uint8_t)(ttl & 0xFFu),
    };
    return append_tlv(buf, cap, pos, LLDP_TLV_TTL, value, sizeof(value));
}

static int append_system_cap_tlv(uint8_t *buf, size_t cap, size_t *pos, uint16_t supported, uint16_t enabled)
{
    if (supported == 0u && enabled == 0u)
    {
        return ERRCODE_SUCCESS;
    }
    uint8_t value[4] = {
        (uint8_t)((supported >> 8) & 0xFFu),
        (uint8_t)(supported & 0xFFu),
        (uint8_t)((enabled >> 8) & 0xFFu),
        (uint8_t)(enabled & 0xFFu),
    };
    return append_tlv(buf, cap, pos, LLDP_TLV_SYSTEM_CAP, value, sizeof(value));
}

static int append_mgmt_addr_tlv(uint8_t *buf, size_t cap, size_t *pos, const lldp_mgmt_addr_t *m)
{
    if (!m || !m->present)
    {
        return ERRCODE_SUCCESS;
    }
    if (m->addr_len == 0u || m->addr_len > LLDP_MGMT_ADDR_MAX_LEN || m->oid_len > LLDP_MGMT_ADDR_MAX_LEN)
    {
        return ERRCODE_FAIL;
    }

    uint8_t value[1u + 1u + LLDP_MGMT_ADDR_MAX_LEN + 1u + 4u + 1u + LLDP_MGMT_ADDR_MAX_LEN];
    size_t v = 0u;
    value[v++] = (uint8_t)(m->addr_len + 1u);
    value[v++] = m->addr_subtype;
    memcpy(value + v, m->addr, m->addr_len);
    v += m->addr_len;
    value[v++] = m->if_subtype;
    if (put_u32(value, sizeof(value), &v, m->if_number) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    value[v++] = m->oid_len;
    if (m->oid_len > 0u)
    {
        memcpy(value + v, m->oid, m->oid_len);
        v += m->oid_len;
    }
    return append_tlv(buf, cap, pos, LLDP_TLV_MGMT_ADDR, value, v);
}

int lldp_packet_build_basic(uint8_t *buf, size_t cap, const lldp_packet_build_info_t *info, size_t *len_out)
{
    if (!buf || !info || !len_out)
    {
        return ERRCODE_FAIL;
    }

    size_t pos = 0u;
    if (append_id_tlv(buf, cap, &pos, LLDP_TLV_CHASSIS_ID, &info->chassis_id) != ERRCODE_SUCCESS ||
        append_id_tlv(buf, cap, &pos, LLDP_TLV_PORT_ID, &info->port_id) != ERRCODE_SUCCESS ||
        append_ttl_tlv(buf, cap, &pos, info->ttl) != ERRCODE_SUCCESS ||
        append_string_tlv(buf, cap, &pos, LLDP_TLV_PORT_DESC, info->port_desc) != ERRCODE_SUCCESS ||
        append_string_tlv(buf, cap, &pos, LLDP_TLV_SYSTEM_NAME, info->system_name) != ERRCODE_SUCCESS ||
        append_string_tlv(buf, cap, &pos, LLDP_TLV_SYSTEM_DESC, info->system_desc) != ERRCODE_SUCCESS ||
        append_system_cap_tlv(buf, cap, &pos, info->caps_supported, info->caps_enabled) != ERRCODE_SUCCESS ||
        append_mgmt_addr_tlv(buf, cap, &pos, info->mgmt_addr) != ERRCODE_SUCCESS ||
        append_tlv(buf, cap, &pos, LLDP_TLV_END, NULL, 0u) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    *len_out = pos;
    return ERRCODE_SUCCESS;
}

void lldp_packet_clear(lldp_packet_t *pkt)
{
    if (pkt)
    {
        memset(pkt, 0, sizeof(*pkt));
    }
}

static int parse_id_tlv(lldp_id_t *id, const uint8_t *value, uint16_t len)
{
    if (!id || !value || len < 2u || (uint16_t)(len - 1u) > LLDP_ID_MAX_LEN)
    {
        return ERRCODE_FAIL;
    }
    id->subtype = value[0];
    id->len = (uint16_t)(len - 1u);
    memcpy(id->data, value + 1u, id->len);
    return ERRCODE_SUCCESS;
}

static void parse_text_tlv(char *dst, size_t dst_len, const uint8_t *value, uint16_t len)
{
    if (!dst || dst_len == 0u)
    {
        return;
    }
    size_t n = len;
    if (n >= dst_len)
    {
        n = dst_len - 1u;
    }
    if (n > 0u && value)
    {
        memcpy(dst, value, n);
    }
    dst[n] = '\0';
}

static int parse_mgmt_addr(lldp_mgmt_addr_t *m, const uint8_t *value, uint16_t len)
{
    if (!m || !value || len < 8u)
    {
        return ERRCODE_FAIL;
    }
    size_t pos = 0u;
    uint8_t addr_string_len = value[pos++];
    if (addr_string_len < 2u)
    {
        return ERRCODE_FAIL;
    }
    uint8_t addr_len = (uint8_t)(addr_string_len - 1u);
    if (addr_len > LLDP_MGMT_ADDR_MAX_LEN || pos + addr_string_len + 6u > len)
    {
        return ERRCODE_FAIL;
    }

    m->present = 1u;
    m->addr_subtype = value[pos++];
    m->addr_len = addr_len;
    memcpy(m->addr, value + pos, addr_len);
    pos += addr_len;
    m->if_subtype = value[pos++];
    m->if_number = get_u32(value + pos);
    pos += 4u;
    m->oid_len = value[pos++];
    if (m->oid_len > LLDP_MGMT_ADDR_MAX_LEN || pos + m->oid_len > len)
    {
        return ERRCODE_FAIL;
    }
    if (m->oid_len > 0u)
    {
        memcpy(m->oid, value + pos, m->oid_len);
    }
    return ERRCODE_SUCCESS;
}

int lldp_packet_parse(const uint8_t *buf, size_t len, lldp_packet_t *out)
{
    if (!buf || !out)
    {
        return ERRCODE_FAIL;
    }

    lldp_packet_clear(out);
    size_t pos = 0u;
    uint8_t have_chassis = 0u;
    uint8_t have_port = 0u;
    uint8_t have_ttl = 0u;

    while (pos + 2u <= len)
    {
        uint16_t header = get_u16(buf + pos);
        pos += 2u;
        uint8_t type = (uint8_t)((header >> 9) & 0x7Fu);
        uint16_t vlen = (uint16_t)(header & 0x01FFu);
        if (pos + vlen > len)
        {
            return ERRCODE_FAIL;
        }

        const uint8_t *value = buf + pos;
        if (type == LLDP_TLV_END)
        {
            if (vlen != 0u)
            {
                return ERRCODE_FAIL;
            }
            out->has_end = 1u;
            return (have_chassis && have_port && have_ttl) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
        }

        switch (type)
        {
            case LLDP_TLV_CHASSIS_ID:
                if (parse_id_tlv(&out->chassis_id, value, vlen) != ERRCODE_SUCCESS)
                {
                    return ERRCODE_FAIL;
                }
                have_chassis = 1u;
                break;
            case LLDP_TLV_PORT_ID:
                if (parse_id_tlv(&out->port_id, value, vlen) != ERRCODE_SUCCESS)
                {
                    return ERRCODE_FAIL;
                }
                have_port = 1u;
                break;
            case LLDP_TLV_TTL:
                if (vlen != 2u)
                {
                    return ERRCODE_FAIL;
                }
                out->ttl = get_u16(value);
                have_ttl = 1u;
                break;
            case LLDP_TLV_PORT_DESC:
                parse_text_tlv(out->port_desc, sizeof(out->port_desc), value, vlen);
                break;
            case LLDP_TLV_SYSTEM_NAME:
                parse_text_tlv(out->system_name, sizeof(out->system_name), value, vlen);
                break;
            case LLDP_TLV_SYSTEM_DESC:
                parse_text_tlv(out->system_desc, sizeof(out->system_desc), value, vlen);
                break;
            case LLDP_TLV_SYSTEM_CAP:
                if (vlen != 4u)
                {
                    return ERRCODE_FAIL;
                }
                out->caps_supported = get_u16(value);
                out->caps_enabled = get_u16(value + 2u);
                break;
            case LLDP_TLV_MGMT_ADDR:
                if (parse_mgmt_addr(&out->mgmt_addr, value, vlen) != ERRCODE_SUCCESS)
                {
                    return ERRCODE_FAIL;
                }
                break;
            default:
                break;
        }

        pos += vlen;
    }

    return ERRCODE_FAIL;
}
