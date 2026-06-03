/**
 * @file   bgp_parse_flowspec.c
 * @brief  AFI=1/2 SAFI=133（FlowSpec）NLRI 处理器（RFC 5575 / RFC 7674）
 * @author jhb
 * @date   2026/03/11
 *
 * FlowSpec NLRI 编码：
 *   NLRI Length (1B if < 240, 2B if >= 240) → 后续字节数
 *   For each component:
 *     Type    (1B)
 *     Value   (type-dependent):
 *       Type 1,2: prefix_len(1B) + prefix_bytes  （同 BGP 前缀编码）
 *       Type 3-12: 一或多个 operator-value 对
 *         Operator (1B): end(7) len(6-5) and(6) lt(3) gt(2) eq(1)
 *         Value   (1-4B，由 operator.len 决定：00=1B 01=2B 10=4B）
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_parse_priv.h"

/* ============================================================================
 * Operator 字节位定义
 * ========================================================================== */

#define FS_OP_END (1U << 7) /* 当前 type 最后一个 value */
#define FS_OP_AND (1U << 6) /* AND 关系（否则 OR） */
#define FS_OP_LEN 0x30U     /* bits 5-4：value 长度编码 */
#define FS_OP_LT (1U << 3)
#define FS_OP_GT (1U << 2)
#define FS_OP_EQ (1U << 1)

/** 从 operator.len 字段解码字节数（0→1, 1→2, 2→4, 3→8） */
static uint8_t op_value_bytes(uint8_t op)
{
    return (uint8_t)(1u << ((op & FS_OP_LEN) >> 4));
}

/* ============================================================================
 * 数值型组件（类型 3-12）→ 字符串
 * ========================================================================== */

static const char *fs_type_name(uint8_t type)
{
    switch (type)
    {
        case BGP_FS_TYPE_PROTO:
            return "proto";
        case BGP_FS_TYPE_PORT:
            return "port";
        case BGP_FS_TYPE_DST_PORT:
            return "dport";
        case BGP_FS_TYPE_SRC_PORT:
            return "sport";
        case BGP_FS_TYPE_ICMP_TYPE:
            return "icmp-type";
        case BGP_FS_TYPE_ICMP_CODE:
            return "icmp-code";
        case BGP_FS_TYPE_TCP_FLAGS:
            return "tcp-flags";
        case BGP_FS_TYPE_PKT_LEN:
            return "pkt-len";
        case BGP_FS_TYPE_DSCP:
            return "dscp";
        case BGP_FS_TYPE_FRAGMENT:
            return "fragment";
        default:
            return "unknown";
    }
}

/** 协议号转名称（仅常见值） */
static const char *proto_name(uint64_t proto)
{
    switch (proto)
    {
        case 1:
            return "icmp";
        case 6:
            return "tcp";
        case 17:
            return "udp";
        case 58:
            return "icmp6";
        case 89:
            return "ospf";
        default:
            return NULL;
    }
}

/** Fragment 标志位转字符串 */
static void fragment_flags_str(uint64_t val, char *buf, size_t sz)
{
    buf[0] = '\0';
    if (val & 0x01)
    {
        strncat(buf, "df ", sz - strlen(buf) - 1);
    }
    if (val & 0x02)
    {
        strncat(buf, "isf ", sz - strlen(buf) - 1);
    }
    if (val & 0x04)
    {
        strncat(buf, "ff ", sz - strlen(buf) - 1);
    }
    if (val & 0x08)
    {
        strncat(buf, "lf ", sz - strlen(buf) - 1);
    }
    /* 去掉末尾空格 */
    size_t l = strlen(buf);
    if (l > 0 && buf[l - 1] == ' ')
    {
        buf[l - 1] = '\0';
    }
}

static void op_val_to_str(uint8_t type, uint8_t op, uint64_t val, bool first, char *buf, size_t sz)
{
    char op_str[8] = "";

    /* AND/OR 拼接 */
    if (!first)
    {
        strncat(op_str, (op & FS_OP_AND) ? "&" : ",", sizeof(op_str) - 1);
    }

    /* 比较符 */
    bool lt = (op & FS_OP_LT) != 0;
    bool gt = (op & FS_OP_GT) != 0;
    bool eq = (op & FS_OP_EQ) != 0;

    /* 拼接比较符，eq+gt/lt 和单独 eq 使用 "=" 是一样的，合并最后两个分支 */
    const char *cmp;
    if (lt && eq)
    {
        cmp = "<=";
    }
    else if (gt && eq)
    {
        cmp = ">=";
    }
    else if (lt)
    {
        cmp = "<";
    }
    else if (gt)
    {
        cmp = ">";
    }
    else
    {
        cmp = "=";
    }

    if (type == BGP_FS_TYPE_PROTO)
    {
        const char *pname = proto_name(val);
        if (pname)
        {
            snprintf(buf, sz, "%s%s%s%s", op_str, fs_type_name(type), cmp, pname);
            return;
        }
    }

    if (type == BGP_FS_TYPE_FRAGMENT)
    {
        char fstr[32];
        fragment_flags_str(val, fstr, sizeof(fstr));
        snprintf(buf, sz, "%s%s%s%s", op_str, fs_type_name(type), cmp, fstr);
        return;
    }

    snprintf(buf, sz, "%s%s%s%llu", op_str, fs_type_name(type), cmp, (unsigned long long)val);
}

/* ============================================================================
 * 解析单个 FlowSpec NLRI 条目的所有组件
 * ========================================================================== */

static bool parse_fs_components(const uint8_t *data, uint16_t len, int af, bgp_nlri_flowspec_t *fs)
{
    uint16_t pos = 0;

    while (pos < len && fs->count < BGP_FS_MAX_COMPONENTS)
    {
        if (pos >= len)
        {
            break;
        }

        uint8_t comp_type = data[pos++];
        bgp_fs_component_t *c = &fs->components[fs->count];
        c->type = comp_type;

        if (comp_type == BGP_FS_TYPE_DST_PREFIX || comp_type == BGP_FS_TYPE_SRC_PREFIX)
        {
            /* 前缀编码（同 BGP NLRI） */
            net_prefix_t pfx;
            uint16_t consumed;
            int paf = (af == AF_INET6) ? AF_INET6 : AF_INET;

            if (bgp_read_prefix(data + pos, len - pos, paf, &pfx, &consumed) < 0)
            {
                return false;
            }

            /* 保存原始字节 */
            uint8_t raw_bytes = (uint8_t)consumed;
            if (raw_bytes <= BGP_FS_COMP_DATA_MAX)
            {
                memcpy(c->data, data + pos, raw_bytes);
                c->data_len = raw_bytes;
            }

            char ip[INET6_ADDRSTRLEN];
            if (paf == AF_INET6)
            {
                inet_ntop(AF_INET6, &pfx.addr.u.v6, ip, sizeof(ip));
            }
            else
            {
                inet_ntop(AF_INET, &pfx.addr.u.v4, ip, sizeof(ip));
            }
            snprintf(c->str, sizeof(c->str), "%s=%s/%u", (comp_type == BGP_FS_TYPE_DST_PREFIX) ? "dst" : "src", ip,
                     pfx.prefix_len);

            pos += consumed;
            fs->count++;
        }
        else
        {
            /* 数值型组件：连续读取 operator-value 对 */
            char comp_str[128] = "";
            bool first = true;
            uint8_t op_start = pos;

            while (pos < len)
            {
                if (pos >= len)
                {
                    break;
                }
                uint8_t op = data[pos++];
                uint8_t vbytes = op_value_bytes(op);

                if (pos + vbytes > len)
                {
                    break;
                }

                /* 读取值（大端，最多 8 字节） */
                uint64_t val = 0;
                for (uint8_t i = 0; i < vbytes; i++)
                {
                    val = (val << 8) | data[pos + i];
                }
                pos += vbytes;

                /* 追加到显示字符串 */
                char part[64];
                op_val_to_str(comp_type, op, val, first, part, sizeof(part));
                strncat(comp_str, part, sizeof(comp_str) - strlen(comp_str) - 1);
                first = false;

                /* 保存原始字节 */
                uint8_t raw_len = (uint8_t)(pos - op_start);
                if (raw_len <= BGP_FS_COMP_DATA_MAX)
                {
                    memcpy(c->data, data + op_start, raw_len);
                    c->data_len = raw_len;
                }

                if (op & FS_OP_END)
                {
                    break;
                }
            }

            snprintf(c->str, sizeof(c->str), "%s", comp_str);
            fs->count++;
        }
    }

    return true;
}

/* ============================================================================
 * 主 FlowSpec NLRI 解析
 * 每条 NLRI 以变长 length 字段开头（< 240 → 1B, >= 240 → 2B）
 * ========================================================================== */

static int parse_flowspec_nlri(const uint8_t *data, uint16_t len, int af, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    /* 预统计条目数 */
    uint16_t pos = 0;
    uint32_t count = 0;

    while (pos < len)
    {
        uint16_t nlri_len;
        if (data[pos] >= 240)
        {
            if (pos + 2 > len)
            {
                break;
            }
            nlri_len = (uint16_t)(((uint16_t)(data[pos] & 0x0F) << 8) | data[pos + 1]);
            pos += 2;
        }
        else
        {
            nlri_len = data[pos++];
        }

        if (pos + nlri_len > len)
        {
            break;
        }

        pos += nlri_len;
        count++;
    }

    if (count == 0)
    {
        return 0;
    }

    *out = calloc(count, sizeof(bgp_nlri_entry_t));
    if (!*out)
    {
        return -1;
    }

    pos = 0;
    uint32_t idx = 0;

    while (pos < len && idx < count)
    {
        uint16_t nlri_len;
        if (data[pos] >= 240)
        {
            nlri_len = (uint16_t)(((uint16_t)(data[pos] & 0x0F) << 8) | data[pos + 1]);
            pos += 2;
        }
        else
        {
            nlri_len = data[pos++];
        }

        if (pos + nlri_len > len)
        {
            break;
        }

        bgp_nlri_entry_t *e = &(*out)[idx];
        e->afi = (af == AF_INET6) ? BGP_AFI_IPV6 : BGP_AFI_IPV4;
        e->safi = BGP_SAFI_FLOWSPEC;
        e->type = BGP_NLRI_FLOWSPEC;

        parse_fs_components(data + pos, nlri_len, af, &e->flowspec);

        pos += nlri_len;
        idx++;
    }

    *out_len = idx;
    return 0;
}

/* ============================================================================
 * AFI 封装
 * ========================================================================== */

static int fs_ipv4_reach(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    return parse_flowspec_nlri(data, len, AF_INET, out, out_len);
}

static int fs_ipv6_reach(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    return parse_flowspec_nlri(data, len, AF_INET6, out, out_len);
}

static int fs_nexthop(const uint8_t *nh_data, uint8_t nh_len, uint32_t flags, bgp_nexthop_t *nexthop)
{
    (void)flags;
    /* FlowSpec 通常无 nexthop，但可能携带 redirect next-hop */
    if (nh_len == 4)
    {
        nexthop->global.family = AF_INET;
        memcpy(&nexthop->global.u.v4, nh_data, 4);
    }
    else if (nh_len >= 16)
    {
        nexthop->global.family = AF_INET6;
        memcpy(nexthop->global.u.v6.s6_addr, nh_data, 16);
    }
    return 0;
}

static void fs_entry_to_str(const bgp_nlri_entry_t *entry, char *buf, size_t sz)
{
    if (!entry || !buf || sz == 0)
    {
        return;
    }

    size_t wpos = 0;
    for (uint8_t i = 0; i < entry->flowspec.count && wpos + 1 < sz; i++)
    {
        const char *part = entry->flowspec.components[i].str;
        if (!part || part[0] == '\0')
        {
            continue;
        }

        if (wpos > 0)
        {
            buf[wpos++] = ',';
        }

        size_t rem = sz - wpos;
        int n = snprintf(buf + wpos, rem, "%s", part);
        if (n < 0)
        {
            break;
        }
        if ((size_t)n >= rem)
        {
            wpos = sz - 1;
            break;
        }
        wpos += (size_t)n;
    }

    if (wpos == 0)
    {
        snprintf(buf, sz, "fs:empty");
    }
    else
    {
        buf[wpos] = '\0';
    }
}

/* ============================================================================
 * 处理器描述符与注册
 * ========================================================================== */

static const bgp_af_parser_t g_fs_ipv4 = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_FLOWSPEC,
    .parse_reach = fs_ipv4_reach,
    .parse_unreach = fs_ipv4_reach,
    .parse_nexthop = fs_nexthop,
    .entry_to_str = fs_entry_to_str,
};

static const bgp_af_parser_t g_fs_ipv6 = {
    .afi = BGP_AFI_IPV6,
    .safi = BGP_SAFI_FLOWSPEC,
    .parse_reach = fs_ipv6_reach,
    .parse_unreach = fs_ipv6_reach,
    .parse_nexthop = fs_nexthop,
    .entry_to_str = fs_entry_to_str,
};

void bgp_parse_flowspec_register(void)
{
    bgp_af_parser_register(&g_fs_ipv4);
    bgp_af_parser_register(&g_fs_ipv6);
}
