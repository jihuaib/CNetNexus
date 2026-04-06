/**
 * @file   bgp_parse_open.c
 * @brief  BGP OPEN 报文解析（完整能力集）及 PDU header 解析
 * @author jhb
 * @date   2026/03/13
 *
 * 支持的能力码（RFC 5492）：
 *   1  = Multiprotocol Extensions (RFC 4760)
 *   2  = Route Refresh (RFC 2918)
 *   6  = Extended Message (RFC 8654)
 *   9  = BGP Role (RFC 9234)
 *  64  = Graceful Restart (RFC 4724)
 *  65  = 4-Octet AS Number (RFC 6793)
 *  69  = ADD-PATH (RFC 7911)
 *  70  = Enhanced Route Refresh (RFC 7313)
 *  73  = FQDN (draft-walton-bgp-hostname-capability)
 */
#include <arpa/inet.h>
#include <string.h>

#include "bgp.h"

/* BGP Marker：16 字节全 0xFF */
static const uint8_t BGP_MARKER[16] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

/* ============================================================================
 * bgp_pdu_parse_hdr
 * ========================================================================== */

int bgp_pdu_parse_hdr(const uint8_t *data, uint32_t data_len, bgp_pdu_info_t *info)
{
    if (!data || !info || data_len < BGP_HEADER_LEN)
    {
        return -1;
    }

    /* Marker 校验 */
    if (memcmp(data, BGP_MARKER, 16) != 0)
    {
        return -1;
    }

    uint16_t msg_len = ((uint16_t)data[16] << 8) | data[17];

    /* 长度合法性（最小 19，最大 4096，RFC 8654 扩展后最大 65535） */
    if (msg_len < BGP_HEADER_LEN || (uint32_t)msg_len > data_len)
    {
        return -1;
    }

    info->msg_type = data[18];
    info->msg_len = msg_len;
    info->body = data + BGP_HEADER_LEN;
    info->body_len = msg_len - BGP_HEADER_LEN;
    return 0;
}

/* ============================================================================
 * 能力解析辅助
 * ========================================================================== */

/** 解析单个能力（cap_code + cap_len + value） */
static void parse_capability(bgp_open_msg_t *msg, uint8_t cap_code, const uint8_t *cap_val, uint8_t cap_len)
{
    switch (cap_code)
    {
        case BGP_CAP_MP_EXTENSIONS: /* AFI(2B) + reserved(1B) + SAFI(1B) */
            if (cap_len >= 4 && msg->mp_count < BGP_OPEN_MAX_MP_CAPS)
            {
                uint16_t afi = ((uint16_t)cap_val[0] << 8) | cap_val[1];
                uint8_t safi = cap_val[3];
                msg->mp_afs[msg->mp_count] = afi;
                msg->mp_safis[msg->mp_count] = safi;
                msg->mp_count++;
            }
            break;

        case BGP_CAP_ROUTE_REFRESH:
            msg->cap_route_refresh = true;
            break;

        case BGP_CAP_EXT_NEXTHOP:
        {
            /* 每条 6B：NLRI-AFI(2B) + NLRI-SAFI(2B) + NH-AFI(2B)（RFC 8950） */
            msg->cap_ext_nexthop = true;
            uint8_t pos = 0;
            while (pos + 6 <= cap_len && msg->ext_nh_count < BGP_OPEN_MAX_EXT_NH)
            {
                bgp_ext_nh_entry_t *e = &msg->ext_nh[msg->ext_nh_count++];
                e->nlri_afi = ((uint16_t)cap_val[pos] << 8) | cap_val[pos + 1];
                e->nlri_safi = ((uint16_t)cap_val[pos + 2] << 8) | cap_val[pos + 3];
                e->nh_afi = ((uint16_t)cap_val[pos + 4] << 8) | cap_val[pos + 5];
                pos += 6;
            }
            break;
        }

        case BGP_CAP_EXT_MESSAGE:
            msg->cap_ext_message = true;
            break;

        case BGP_CAP_BGP_ROLE: /* role(1B) */
            if (cap_len >= 1)
            {
                msg->cap_bgp_role = cap_val[0];
            }
            break;

        case BGP_CAP_GRACEFUL_RESTART:
        {
            /* 2B: Restart Flags(4b) + Restart Time(12b) */
            if (cap_len < 2)
            {
                break;
            }
            msg->cap_graceful_restart = true;
            bgp_cap_gr_t *gr = &msg->graceful_restart;

            uint16_t rw = ((uint16_t)cap_val[0] << 8) | cap_val[1];
            gr->restart_flag = (rw & 0x8000) != 0;
            gr->notification_flag = (rw & 0x4000) != 0;
            gr->restart_time = rw & 0x0FFF;

            /* 后续为每个 AF：AFI(2B) + SAFI(1B) + Flags(1B) */
            uint8_t pos = 2;
            while (pos + 4 <= cap_len && gr->af_count < BGP_OPEN_MAX_GR_AFS)
            {
                bgp_gr_af_t *af = &gr->afs[gr->af_count++];
                af->afi = ((uint16_t)cap_val[pos] << 8) | cap_val[pos + 1];
                af->safi = cap_val[pos + 2];
                af->forwarding_state = (cap_val[pos + 3] & 0x80) != 0;
                pos += 4;
            }
            break;
        }

        case BGP_CAP_AS4: /* 4-octet AS(4B) */
            if (cap_len >= 4)
            {
                msg->cap_as4 = ((uint32_t)cap_val[0] << 24) | ((uint32_t)cap_val[1] << 16) |
                               ((uint32_t)cap_val[2] << 8) | cap_val[3];
            }
            break;

        case BGP_CAP_ADD_PATH:
        {
            /* 每条 4B：AFI(2B) + SAFI(1B) + Send/Receive(1B) */
            msg->cap_add_path = true;
            uint8_t pos = 0;
            while (pos + 4 <= cap_len && msg->add_path_count < BGP_OPEN_MAX_ADDPATH)
            {
                bgp_addpath_af_t *ap = &msg->add_path[msg->add_path_count++];
                ap->afi = ((uint16_t)cap_val[pos] << 8) | cap_val[pos + 1];
                ap->safi = cap_val[pos + 2];
                ap->flags = cap_val[pos + 3];
                pos += 4;
            }
            break;
        }

        case BGP_CAP_ENHANCED_RR:
            msg->cap_enhanced_rr = true;
            break;

        case BGP_CAP_FQDN:
        {
            /* hostname_len(1B) + hostname + domain_len(1B) + domain */
            if (cap_len < 1)
            {
                break;
            }
            uint8_t hn_len = cap_val[0];
            if (1 + hn_len > cap_len)
            {
                break;
            }
            uint8_t copy_hn = hn_len < sizeof(msg->hostname) - 1 ? hn_len : (uint8_t)(sizeof(msg->hostname) - 1);
            memcpy(msg->hostname, cap_val + 1, copy_hn);
            msg->hostname[copy_hn] = '\0';

            uint8_t dn_off = (uint8_t)(1 + hn_len);
            if (dn_off >= cap_len)
            {
                break;
            }
            uint8_t dn_len = cap_val[dn_off];
            if (dn_off + 1 + dn_len > cap_len)
            {
                break;
            }
            uint8_t copy_dn = dn_len < sizeof(msg->domain) - 1 ? dn_len : (uint8_t)(sizeof(msg->domain) - 1);
            memcpy(msg->domain, cap_val + dn_off + 1, copy_dn);
            msg->domain[copy_dn] = '\0';
            break;
        }

        default:
            /* 未知能力：忽略 */
            break;
    }
}

/* ============================================================================
 * bgp_open_parse
 * ========================================================================== */

int bgp_open_parse(const uint8_t *body, uint16_t body_len, bgp_open_msg_t *msg)
{
    if (!body || !msg)
    {
        return -1;
    }

    memset(msg, 0, sizeof(*msg));
    msg->cap_bgp_role = 0xFF; /* 0xFF = 未携带 */

    /* 最小 OPEN 报文体：version(1)+as(2)+hold(2)+bgp-id(4)+opt-len(1) = 10B */
    if (body_len < 10)
    {
        return -1;
    }

    msg->version = body[0];
    msg->my_as = ((uint16_t)body[1] << 8) | body[2];
    msg->hold_time = ((uint16_t)body[3] << 8) | body[4];

    struct in_addr bgp_id;
    memcpy(&bgp_id, body + 5, 4);
    inet_ntop(AF_INET, &bgp_id, msg->bgp_id, sizeof(msg->bgp_id));

    if (msg->version != 4)
    {
        return -1;
    }

    /* ---- 解析 Optional Parameters ---- */
    uint8_t opt_len = body[9];
    uint16_t pos = 10;
    uint16_t opt_end = (uint16_t)(10 + opt_len);

    if (opt_end > body_len)
    {
        opt_end = body_len; /* 容错：截断至实际长度 */
    }

    while (pos + 2 <= opt_end)
    {
        uint8_t param_type = body[pos];
        uint8_t param_len = body[pos + 1];
        pos += 2;

        if (pos + param_len > opt_end)
        {
            break; /* 参数超出边界 */
        }

        if (param_type == 2) /* Capabilities Optional Parameter */
        {
            uint16_t cap_end = pos + param_len;

            while (pos + 2 <= cap_end)
            {
                uint8_t cap_code = body[pos];
                uint8_t cap_len = body[pos + 1];
                pos += 2;

                if (pos + cap_len > cap_end)
                {
                    break;
                }

                parse_capability(msg, cap_code, body + pos, cap_len);
                pos += cap_len;
            }
        }
        else
        {
            pos += param_len;
        }
    }

    return 0;
}
