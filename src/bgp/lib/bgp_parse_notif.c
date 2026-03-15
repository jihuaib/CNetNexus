/**
 * @file   bgp_parse_notif.c
 * @brief  BGP NOTIFICATION 报文解析及错误码描述
 * @author jhb
 * @date   2026/03/13
 */
#include <string.h>

#include "bgp.h"

/* ============================================================================
 * bgp_notif_error_str
 * ========================================================================== */

const char *bgp_notif_error_str(uint8_t code, uint8_t subcode)
{
    switch (code)
    {
        case BGP_ERR_MSG_HDR:
            switch (subcode)
            {
                case 1:
                    return "Message-Header/Connection-Not-Synchronized";
                case 2:
                    return "Message-Header/Bad-Message-Length";
                case 3:
                    return "Message-Header/Bad-Message-Type";
                default:
                    return "Message-Header/Unknown";
            }

        case BGP_ERR_OPEN:
            switch (subcode)
            {
                case BGP_OPEN_ERR_UNSUPPORTED_VERSION:
                    return "Open/Unsupported-Version";
                case BGP_OPEN_ERR_BAD_PEER_AS:
                    return "Open/Bad-Peer-AS";
                case BGP_OPEN_ERR_BAD_BGP_ID:
                    return "Open/Bad-BGP-Identifier";
                case BGP_OPEN_ERR_UNSUPPORTED_OPT:
                    return "Open/Unsupported-Optional-Parameter";
                case BGP_OPEN_ERR_UNACCEPTABLE_HOLD:
                    return "Open/Unacceptable-Hold-Time";
                case BGP_OPEN_ERR_UNSUPPORTED_CAP:
                    return "Open/Unsupported-Capability";
                default:
                    return "Open/Unknown";
            }

        case BGP_ERR_UPDATE:
            switch (subcode)
            {
                case 1:
                    return "Update/Malformed-Attribute-List";
                case 2:
                    return "Update/Unrecognized-Well-Known-Attribute";
                case 3:
                    return "Update/Missing-Well-Known-Attribute";
                case 4:
                    return "Update/Attribute-Flags-Error";
                case 5:
                    return "Update/Attribute-Length-Error";
                case 6:
                    return "Update/Invalid-ORIGIN-Attribute";
                case 8:
                    return "Update/Invalid-NEXT-HOP-Attribute";
                case 9:
                    return "Update/Optional-Attribute-Error";
                case 10:
                    return "Update/Invalid-Network-Field";
                case 11:
                    return "Update/Malformed-AS-PATH";
                default:
                    return "Update/Unknown";
            }

        case BGP_ERR_HOLD_TIMER:
            return "Hold-Timer-Expired";

        case BGP_ERR_FSM:
            switch (subcode)
            {
                case 1:
                    return "FSM/Unexpected-Message-in-OpenSent";
                case 2:
                    return "FSM/Unexpected-Message-in-OpenConfirm";
                case 3:
                    return "FSM/Unexpected-Message-in-Established";
                default:
                    return "FSM/Unknown";
            }

        case BGP_ERR_CEASE:
            switch (subcode)
            {
                case BGP_CEASE_MAX_PREFIXES:
                    return "Cease/Maximum-Prefixes-Reached";
                case BGP_CEASE_ADMIN_SHUTDOWN:
                    return "Cease/Admin-Shutdown";
                case BGP_CEASE_PEER_DECONFIG:
                    return "Cease/Peer-Deconfigured";
                case BGP_CEASE_ADMIN_RESET:
                    return "Cease/Admin-Reset";
                case BGP_CEASE_CONN_REJECTED:
                    return "Cease/Connection-Rejected";
                case BGP_CEASE_OTHER_CONFIG:
                    return "Cease/Other-Config-Change";
                case BGP_CEASE_CONN_COLLISION:
                    return "Cease/Connection-Collision-Resolution";
                case BGP_CEASE_OUT_OF_RESOURCES:
                    return "Cease/Out-Of-Resources";
                case BGP_CEASE_HARD_RESET:
                    return "Cease/Hard-Reset";
                default:
                    return "Cease/Unknown";
            }

        default:
            return "Unknown";
    }
}

/* ============================================================================
 * bgp_notif_parse
 * ========================================================================== */

int bgp_notif_parse(const uint8_t *body, uint16_t body_len, bgp_notif_msg_t *msg)
{
    if (!body || !msg)
    {
        return -1;
    }

    memset(msg, 0, sizeof(*msg));

    /* error_code(1B) + error_subcode(1B) 为最短合法报文体 */
    if (body_len >= 1)
    {
        msg->error_code = body[0];
    }
    if (body_len >= 2)
    {
        msg->error_subcode = body[1];
    }

    /* 附带数据（可选） */
    if (body_len > 2)
    {
        uint16_t data_len = body_len - 2;
        if (data_len > BGP_NOTIF_DATA_MAX)
        {
            data_len = BGP_NOTIF_DATA_MAX;
        }
        memcpy(msg->data, body + 2, data_len);
        msg->data_len = data_len;
    }

    /* 生成可读描述 */
    const char *desc = bgp_notif_error_str(msg->error_code, msg->error_subcode);
    strncpy(msg->error_str, desc, sizeof(msg->error_str) - 1);
    msg->error_str[sizeof(msg->error_str) - 1] = '\0';

    return 0;
}
