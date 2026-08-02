/**
 * @file   isis_lsp.c
 * @brief  ISIS LSP 报文发送、接收与前缀路由学习（IPv4/IPv6）
 * @author jhb
 * @date   2026/04/12
 */
#include "isis_lsp.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "if.h"
#include "isis.h"
#include "isis_main.h"
#include "isis_spf.h"
#include "log.h"
#include "route.h"

#define ISIS_NLPID 0x83u
#define ISIS_LLC_DSAP 0xFEu
#define ISIS_LLC_SSAP 0xFEu
#define ISIS_LLC_CTRL 0x03u

#define ISIS_PDU_TYPE_LSP_L1 18u
#define ISIS_PDU_TYPE_LSP_L2 20u
#define ISIS_LSP_HDR_LEN 27u
#define ISIS_LSP_CKSUM_COVER_OFFSET 12u
#define ISIS_LSP_CKSUM_FIELD_OFFSET 24u
#define ISIS_LSP_CKSUM_FIELD_REL_OFFSET (ISIS_LSP_CKSUM_FIELD_OFFSET - ISIS_LSP_CKSUM_COVER_OFFSET)

#define ISIS_TLV_AREA_ADDR 1u             /* ISO 10589 Area Addresses */
#define ISIS_TLV_IS_REACH 2u              /* RFC 1195 narrow IS reach */
#define ISIS_TLV_IP_INT_REACH 128u        /* RFC 1195 narrow IPv4 internal reach */
#define ISIS_TLV_PROTOCOLS_SUPPORTED 129u /* RFC 1195 NLPID list */
#define ISIS_TLV_IPV4_INTF_ADDR 132u      /* RFC 1195 IPv4 interface address */
#define ISIS_TLV_EXT_IS_REACH 22u         /* RFC 5305 wide IS reach */
#define ISIS_TLV_EXT_IP_REACH 135u        /* RFC 5305 wide IPv4 reach */
#define ISIS_TLV_IPV6_REACH 236u          /* RFC 5308 IPv6 reach */

#define ISIS_NLPID_IPV4 0xCCu
#define ISIS_NLPID_IPV6 0x8Eu

#define ISIS_LSP_PKT_MAX 1500u
#define ISIS_LSP_TX_INTERVAL_SEC 10u
#define ISIS_LSP_DEFAULT_LIFETIME_SEC 120u
#define ISIS_LSP_SEQ_RESTART_GRACE_MSEC (ISIS_LSP_TX_INTERVAL_SEC * 3000u)

static const uint8_t g_isis_l1_dst_mac[ETH_ALEN] = {0x01u, 0x80u, 0xC2u, 0x00u, 0x00u, 0x14u};
static const uint8_t g_isis_l2_dst_mac[ETH_ALEN] = {0x01u, 0x80u, 0xC2u, 0x00u, 0x00u, 0x15u};

typedef struct isis_lsp_rx_ctx
{
    int raw_fd;
    const char *ifname;
    const if_api_cache_entry_t *if_entry;
    const uint8_t *pdu;
    size_t pdu_len;
    const uint8_t *tlvs;
    size_t tlv_len;
    uint8_t level;
    uint8_t system_id[6];
    uint8_t pseudonode_id;
    uint8_t fragment_id;
    uint8_t src_mac[ETH_ALEN];
    uint16_t lifetime_sec;
    uint16_t checksum;
    uint32_t seq;
    uint64_t now_msec;
} isis_lsp_rx_ctx_t;

typedef struct isis_lsp_adv_ctx
{
    const isis_instance_cfg_t *inst;
    uint8_t level;
    GByteArray *is_entries;
    GByteArray *v4_entries;
    GByteArray *v6_entries;
} isis_lsp_adv_ctx_t;

typedef struct isis_lsp_send_if_ctx
{
    const isis_instance_cfg_t *inst;
    int raw_fd;
    uint8_t *pdu_l1;
    size_t pdu_l1_len;
    uint8_t *pdu_l2;
    size_t pdu_l2_len;
    uint32_t sent_l1;
    uint32_t sent_l2;
} isis_lsp_send_if_ctx_t;

static uint64_t isis_lsp_now_msec(void)
{
    return (uint64_t)(g_get_monotonic_time() / 1000);
}

static int isis_lsp_mac_is_zero(const uint8_t mac[ETH_ALEN])
{
    if (!mac)
    {
        return 1;
    }
    for (size_t i = 0u; i < ETH_ALEN; ++i)
    {
        if (mac[i] != 0u)
        {
            return 0;
        }
    }
    return 1;
}

static int isis_lsp_mac_equal(const uint8_t a[ETH_ALEN], const uint8_t b[ETH_ALEN])
{
    if (!a || !b)
    {
        return 0;
    }
    return memcmp(a, b, ETH_ALEN) == 0 ? 1 : 0;
}

static int isis_level_enabled(const isis_instance_cfg_t *inst, uint8_t level)
{
    if (!inst)
    {
        return 0;
    }

    if (level == 1u)
    {
        return (inst->is_type == ISIS_IS_TYPE_LEVEL_1 || inst->is_type == ISIS_IS_TYPE_LEVEL_1_2) ? 1 : 0;
    }
    if (level == 2u)
    {
        return (inst->is_type == ISIS_IS_TYPE_LEVEL_2 || inst->is_type == ISIS_IS_TYPE_LEVEL_1_2) ? 1 : 0;
    }
    return 0;
}

static int isis_lsp_instance_af_enabled(const isis_instance_cfg_t *inst, uint16_t afi)
{
    if (!inst)
    {
        return 0;
    }
    if (afi == ISIS_AFI_IPV4)
    {
        return inst->af_ipv4 ? 1 : 0;
    }
    if (afi == ISIS_AFI_IPV6)
    {
        return inst->af_ipv6 ? 1 : 0;
    }
    return 0;
}

static const isis_if_af_cfg_t *isis_lsp_if_af_cfg(const isis_instance_cfg_t *inst, const isis_if_cfg_t *if_cfg,
                                                  uint16_t afi)
{
    if (!inst || !if_cfg || !isis_lsp_instance_af_enabled(inst, afi))
    {
        return NULL;
    }
    const isis_if_af_cfg_t *af_cfg = isis_if_cfg_af_const(if_cfg, afi);
    if (!af_cfg || !af_cfg->enabled)
    {
        return NULL;
    }
    return af_cfg;
}

static const isis_if_af_cfg_t *isis_lsp_pick_active_af_cfg(const isis_instance_cfg_t *inst, const isis_if_cfg_t *if_cfg)
{
    const isis_if_af_cfg_t *af_cfg_v4 = isis_lsp_if_af_cfg(inst, if_cfg, ISIS_AFI_IPV4);
    if (af_cfg_v4 && !af_cfg_v4->passive)
    {
        return af_cfg_v4;
    }

    const isis_if_af_cfg_t *af_cfg_v6 = isis_lsp_if_af_cfg(inst, if_cfg, ISIS_AFI_IPV6);
    if (af_cfg_v6 && !af_cfg_v6->passive)
    {
        return af_cfg_v6;
    }

    return NULL;
}

static int isis_lsp_if_has_up_adjacency(const isis_instance_cfg_t *inst, const char *ifname, uint8_t level)
{
    if (!inst || !inst->neighbors || !ifname || ifname[0] == '\0')
    {
        return 0;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        const isis_neighbor_t *nbr = (const isis_neighbor_t *)value;
        if (!nbr)
        {
            continue;
        }
        if (nbr->level == level && nbr->state == ISIS_ADJ_STATE_UP && strcmp(nbr->ifname, ifname) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int isis_hex_to_nibble(int c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

static int isis_parse_net_bytes(const char *net, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!net || !out || out_cap == 0 || !out_len)
    {
        return -1;
    }

    size_t len = 0;
    int high_nibble = -1;

    for (const char *p = net; *p != '\0'; ++p)
    {
        if (*p == '.' || *p == ':' || *p == '-' || isspace((unsigned char)*p))
        {
            continue;
        }

        int v = isis_hex_to_nibble(*p);
        if (v < 0)
        {
            return -1;
        }

        if (high_nibble < 0)
        {
            high_nibble = v;
            continue;
        }

        if (len >= out_cap)
        {
            return -1;
        }
        out[len++] = (uint8_t)((high_nibble << 4) | v);
        high_nibble = -1;
    }

    if (high_nibble >= 0 || len == 0)
    {
        return -1;
    }

    *out_len = len;
    return 0;
}

static int isis_extract_system_id(const char *net, uint8_t sysid[6])
{
    uint8_t bytes[64];
    size_t len = 0;
    if (isis_parse_net_bytes(net, bytes, sizeof(bytes), &len) != 0 || len < 7u)
    {
        return -1;
    }

    memcpy(sysid, &bytes[len - 7u], 6u);
    return 0;
}

static void isis_sysid_to_hex(const uint8_t sysid[6], char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    if (!sysid)
    {
        g_strlcpy(buf, "000000000000", sz);
        return;
    }
    g_snprintf(buf, sz, "%02x%02x%02x%02x%02x%02x", sysid[0], sysid[1], sysid[2], sysid[3], sysid[4], sysid[5]);
}

static void isis_lsp_lsdb_key_format(char *buf, size_t sz, uint8_t level, const uint8_t sysid[6], uint8_t pseudonode_id,
                                     uint8_t fragment_id)
{
    if (!buf || sz == 0)
    {
        return;
    }
    char sysid_hex[13] = {0};
    isis_sysid_to_hex(sysid, sysid_hex, sizeof(sysid_hex));
    g_snprintf(buf, sz, "%u|%s|%02x|%02x", (unsigned)level, sysid_hex, (unsigned)pseudonode_id, (unsigned)fragment_id);
}

static void isis_lsp_count_reach_prefixes(const uint8_t *tlvs, size_t tlv_len, uint32_t *v4_cnt, uint32_t *v6_cnt)
{
    if (v4_cnt)
    {
        *v4_cnt = 0u;
    }
    if (v6_cnt)
    {
        *v6_cnt = 0u;
    }
    if (!tlvs || tlv_len == 0u)
    {
        return;
    }

    size_t pos = 0;
    while (pos + 2u <= tlv_len)
    {
        uint8_t tlv_type = tlvs[pos];
        uint8_t len = tlvs[pos + 1u];
        pos += 2u;
        if (pos + len > tlv_len)
        {
            break;
        }

        if (tlv_type == ISIS_TLV_IP_INT_REACH)
        {
            /* RFC 1195 TLV 128 entry：12 字节固定（metric(4) + ip(4) + mask(4)） */
            size_t epos = 0;
            while (epos + 12u <= len)
            {
                if (v4_cnt)
                {
                    (*v4_cnt)++;
                }
                epos += 12u;
            }
        }
        else if (tlv_type == ISIS_TLV_EXT_IP_REACH)
        {
            /* RFC 5305 TLV 135 entry：metric(4) + ctrl(1) + prefix(N) [+ sub-TLV len(1) + sub-TLVs] */
            size_t epos = 0;
            while (epos + 5u <= len)
            {
                uint8_t ctrl = tlvs[pos + epos + 4u];
                uint8_t prefix_len = (uint8_t)(ctrl & 0x3Fu);
                uint8_t has_sub = (uint8_t)((ctrl & 0x40u) != 0u);
                uint8_t pfx_bytes = (uint8_t)((prefix_len + 7u) / 8u);
                size_t entry_len = 5u + pfx_bytes;
                if (prefix_len > 32u || pfx_bytes > 4u || epos + entry_len > len)
                {
                    break;
                }
                if (has_sub)
                {
                    if (epos + entry_len + 1u > len)
                    {
                        break;
                    }
                    uint8_t sub_len = tlvs[pos + epos + entry_len];
                    entry_len += 1u + (size_t)sub_len;
                    if (epos + entry_len > len)
                    {
                        break;
                    }
                }
                if (v4_cnt)
                {
                    (*v4_cnt)++;
                }
                epos += entry_len;
            }
        }
        else if (tlv_type == ISIS_TLV_IPV6_REACH)
        {
            /* RFC 5308 TLV 236 entry：metric(4) + ctrl(1) + prefix_len(1) + prefix(N) [+ sub-TLV len(1) + sub-TLVs] */
            size_t epos = 0;
            while (epos + 6u <= len)
            {
                uint8_t ctrl = tlvs[pos + epos + 4u];
                uint8_t prefix_len = tlvs[pos + epos + 5u];
                uint8_t has_sub = (uint8_t)((ctrl & 0x20u) != 0u);
                uint8_t pfx_bytes = (uint8_t)((prefix_len + 7u) / 8u);
                size_t entry_len = 6u + pfx_bytes;
                if (prefix_len > 128u || pfx_bytes > 16u || epos + entry_len > len)
                {
                    break;
                }
                if (has_sub)
                {
                    if (epos + entry_len + 1u > len)
                    {
                        break;
                    }
                    uint8_t sub_len = tlvs[pos + epos + entry_len];
                    entry_len += 1u + (size_t)sub_len;
                    if (epos + entry_len > len)
                    {
                        break;
                    }
                }
                if (v6_cnt)
                {
                    (*v6_cnt)++;
                }
                epos += entry_len;
            }
        }
        pos += len;
    }
}

static int isis_lsp_checksum_fill(uint8_t *buf, size_t len, size_t checksum_off)
{
    if (!buf || len == 0u || checksum_off + 1u >= len)
    {
        return -1;
    }

    buf[checksum_off] = 0u;
    buf[checksum_off + 1u] = 0u;

    int c0 = 0;
    int c1 = 0;
    for (size_t i = 0u; i < len; ++i)
    {
        c0 = (c0 + buf[i]) % 255;
        c1 = (c1 + c0) % 255;
    }

    int x = (((int)(len - checksum_off - 1u) * c0) - c1) % 255;
    if (x <= 0)
    {
        x += 255;
    }

    int y = (c1 - ((int)(len - checksum_off) * c0)) % 255;
    if (y <= 0)
    {
        y += 255;
    }

    buf[checksum_off] = (uint8_t)x;
    buf[checksum_off + 1u] = (uint8_t)y;
    return 0;
}

static int isis_lsp_checksum_valid(const uint8_t *pdu, size_t pdu_len, uint16_t lifetime_sec)
{
    if (!pdu || pdu_len <= ISIS_LSP_CKSUM_FIELD_OFFSET)
    {
        return 0;
    }

    if (lifetime_sec == 0u)
    {
        return 1;
    }

    const uint8_t *covered = pdu + ISIS_LSP_CKSUM_COVER_OFFSET;
    size_t covered_len = pdu_len - ISIS_LSP_CKSUM_COVER_OFFSET;
    if (covered_len <= ISIS_LSP_CKSUM_FIELD_REL_OFFSET + 1u)
    {
        return 0;
    }

    if (covered[ISIS_LSP_CKSUM_FIELD_REL_OFFSET] == 0u && covered[ISIS_LSP_CKSUM_FIELD_REL_OFFSET + 1u] == 0u)
    {
        return 0;
    }

    int c0 = 0;
    int c1 = 0;
    for (size_t i = 0u; i < covered_len; ++i)
    {
        c0 = (c0 + covered[i]) % 255;
        c1 = (c1 + c0) % 255;
    }

    return (c0 == 0 && c1 == 0) ? 1 : 0;
}

static int isis_get_src_mac(int fd, const if_api_cache_entry_t *if_entry, uint8_t out_mac[ETH_ALEN])
{
    if (fd < 0 || !if_entry || !out_mac)
    {
        return -1;
    }

    char os_ifname[IFNAMSIZ] = {0};
    if (if_entry->physical_name[0] != '\0')
    {
        g_strlcpy(os_ifname, if_entry->physical_name, sizeof(os_ifname));
    }
    else if (if_entry->ifindex != 0u)
    {
        (void)if_indextoname(if_entry->ifindex, os_ifname);
    }

    if (os_ifname[0] == '\0')
    {
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    g_strlcpy(ifr.ifr_name, os_ifname, sizeof(ifr.ifr_name));
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0)
    {
        return -1;
    }

    memcpy(out_mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
    return 0;
}

static int isis_lsp_send_pdu_on_if(int raw_fd, const if_api_cache_entry_t *if_entry, uint8_t level, const uint8_t *pdu,
                                   size_t pdu_len)
{
    if (raw_fd < 0 || !if_entry || if_entry->ifindex == 0u || !pdu || pdu_len == 0u)
    {
        return -1;
    }

    uint8_t src_mac[ETH_ALEN];
    if (isis_get_src_mac(raw_fd, if_entry, src_mac) != 0)
    {
        return -1;
    }

    const uint8_t *dst_mac = (level == 1u) ? g_isis_l1_dst_mac : g_isis_l2_dst_mac;

    uint8_t frame[ISIS_LSP_PKT_MAX + ETH_HLEN + 3u];
    size_t f = 0;
    memcpy(&frame[f], dst_mac, ETH_ALEN);
    f += ETH_ALEN;
    memcpy(&frame[f], src_mac, ETH_ALEN);
    f += ETH_ALEN;

    uint16_t len8023 = htons((uint16_t)(3u + pdu_len));
    memcpy(&frame[f], &len8023, sizeof(len8023));
    f += sizeof(len8023);

    frame[f++] = ISIS_LLC_DSAP;
    frame[f++] = ISIS_LLC_SSAP;
    frame[f++] = ISIS_LLC_CTRL;
    memcpy(&frame[f], pdu, pdu_len);
    f += pdu_len;

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_802_2);
    sll.sll_ifindex = (int)if_entry->ifindex;
    sll.sll_halen = ETH_ALEN;
    memcpy(sll.sll_addr, dst_mac, ETH_ALEN);

    if (sendto(raw_fd, frame, f, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_WARN("ISIS: send LSP failed on %s(ifindex=%u): %s", if_entry->logical_name, if_entry->ifindex,
                     strerror(errno));
        }
        return -1;
    }
    return 0;
}

/* 从 NET 字符串里抽取 area 字节（NET 末 7 字节是 sysid+selector，其余前缀就是 area） */
static int isis_lsp_extract_area(const char *net, uint8_t *area, size_t area_cap, uint8_t *area_len)
{
    uint8_t bytes[64];
    size_t len = 0u;
    if (!area || !area_len)
    {
        return -1;
    }
    if (isis_parse_net_bytes(net, bytes, sizeof(bytes), &len) != 0 || len < 7u)
    {
        return -1;
    }
    size_t n = len - 7u;
    if (n == 0u || n > area_cap || n > 255u)
    {
        return -1;
    }
    memcpy(area, bytes, n);
    *area_len = (uint8_t)n;
    return 0;
}

/* 单 TLV 直接写入（type + len + val），val_len 最大 255 */
static int isis_lsp_append_single_tlv(uint8_t *pdu, size_t cap, size_t *len_io, uint8_t type, const uint8_t *val,
                                      uint8_t val_len)
{
    if (!pdu || !len_io)
    {
        return -1;
    }
    size_t pos = *len_io;
    if (pos + 2u + (size_t)val_len > cap)
    {
        return -1;
    }
    pdu[pos++] = type;
    pdu[pos++] = val_len;
    if (val_len > 0u && val)
    {
        memcpy(&pdu[pos], val, val_len);
        pos += val_len;
    }
    *len_io = pos;
    return 0;
}

static int isis_lsp_append_tlv_chunks(uint8_t *pdu, size_t cap, size_t *len_io, uint8_t tlv_type, const uint8_t *val,
                                      size_t val_len)
{
    if (!pdu || !len_io)
    {
        return -1;
    }

    size_t pos = *len_io;
    size_t done = 0;
    while (done < val_len)
    {
        size_t chunk = val_len - done;
        if (chunk > 255u)
        {
            chunk = 255u;
        }
        if (pos + 2u + chunk > cap)
        {
            return -1;
        }
        pdu[pos++] = tlv_type;
        pdu[pos++] = (uint8_t)chunk;
        memcpy(&pdu[pos], val + done, chunk);
        pos += chunk;
        done += chunk;
    }

    *len_io = pos;
    return 0;
}

/* TLV 2 (narrow IS reach) 分块器：每个 TLV 体 = 1 字节 Virtual Flag + N*11 字节 entries
 * 按 11 字节 entry 对齐拆分，TLV body 上限 255 字节，每块都重新写一次 Virtual Flag */
static int isis_lsp_append_tlv2_chunks(uint8_t *pdu, size_t cap, size_t *len_io, const uint8_t *entries,
                                       size_t entries_len)
{
    if (!pdu || !len_io)
    {
        return -1;
    }
    if (entries_len == 0u)
    {
        return 0;
    }
    if ((entries_len % 11u) != 0u)
    {
        return -1;
    }

    size_t pos = *len_io;
    size_t done = 0;
    const size_t body_avail = 254u; /* 255 - 1 virtual flag */
    while (done < entries_len)
    {
        size_t remaining = entries_len - done;
        size_t this_bytes = (remaining < body_avail) ? remaining : (body_avail - (body_avail % 11u));
        this_bytes -= this_bytes % 11u;
        if (this_bytes == 0u)
        {
            return -1;
        }
        size_t body_len = 1u + this_bytes;
        if (pos + 2u + body_len > cap)
        {
            return -1;
        }
        pdu[pos++] = ISIS_TLV_IS_REACH;
        pdu[pos++] = (uint8_t)body_len;
        pdu[pos++] = 0u; /* virtual flag */
        memcpy(&pdu[pos], entries + done, this_bytes);
        pos += this_bytes;
        done += this_bytes;
    }
    *len_io = pos;
    return 0;
}

/* 把 wide metric 截断到 narrow 6-bit 范围 */
static uint8_t isis_narrow_clamp_metric(uint32_t metric)
{
    if (metric == 0u)
    {
        metric = ISIS_DEFAULT_IF_METRIC;
    }
    if (metric > ISIS_NARROW_MAX_METRIC)
    {
        return (uint8_t)ISIS_NARROW_MAX_METRIC;
    }
    return (uint8_t)metric;
}

/* narrow TLV 128 entry：metric(4) + ip(4) + mask(4) = 12 bytes */
static int isis_lsp_append_reach_entry_narrow_v4(GByteArray *arr, const net_addr_t *prefix_addr, uint8_t prefix_len,
                                                 uint32_t metric)
{
    if (!arr || !prefix_addr || prefix_addr->family != AF_INET || prefix_len > 32u)
    {
        return -1;
    }

    uint8_t entry[12];
    /* default metric：bit7=S(supported=0), bit6=I/E(internal=0), bits5..0 = value */
    entry[0] = (uint8_t)(isis_narrow_clamp_metric(metric) & 0x3Fu);
    /* delay/expense/error metrics：bit7=S=1 (not supported) */
    entry[1] = 0x80u;
    entry[2] = 0x80u;
    entry[3] = 0x80u;
    memcpy(&entry[4], &prefix_addr->u.v4.s_addr, 4u);
    uint32_t mask = (prefix_len == 0u) ? 0u : (0xFFFFFFFFu << (32u - prefix_len));
    entry[8] = (uint8_t)((mask >> 24) & 0xFFu);
    entry[9] = (uint8_t)((mask >> 16) & 0xFFu);
    entry[10] = (uint8_t)((mask >> 8) & 0xFFu);
    entry[11] = (uint8_t)(mask & 0xFFu);
    g_byte_array_append(arr, entry, sizeof(entry));
    return 0;
}

static int isis_lsp_append_reach_entry(GByteArray *arr, const net_addr_t *prefix_addr, uint8_t prefix_len,
                                       uint32_t metric)
{
    if (!arr || !prefix_addr)
    {
        return -1;
    }

    uint8_t pfx_bytes = (uint8_t)((prefix_len + 7u) / 8u);
    if ((prefix_addr->family == AF_INET && prefix_len > 32u) || (prefix_addr->family == AF_INET6 && prefix_len > 128u))
    {
        return -1;
    }
    if (prefix_addr->family == AF_INET && pfx_bytes > 4u)
    {
        return -1;
    }
    if (prefix_addr->family == AF_INET6 && pfx_bytes > 16u)
    {
        return -1;
    }

    uint8_t hdr[6];
    size_t hdr_len = 0u;
    hdr[0] = (uint8_t)((metric >> 24) & 0xFFu);
    hdr[1] = (uint8_t)((metric >> 16) & 0xFFu);
    hdr[2] = (uint8_t)((metric >> 8) & 0xFFu);
    hdr[3] = (uint8_t)(metric & 0xFFu);
    if (prefix_addr->family == AF_INET)
    {
        /* RFC 5305 TLV 135: byte4 = U(1)|S(1)|prefix_len(6)，无独立 prefix_len 字段 */
        hdr[4] = (uint8_t)(prefix_len & 0x3Fu);
        hdr_len = 5u;
    }
    else
    {
        /* RFC 5308 TLV 236: byte4 = U|X|S|rsv，byte5 = prefix_len */
        hdr[4] = 0u;
        hdr[5] = prefix_len;
        hdr_len = 6u;
    }
    g_byte_array_append(arr, hdr, hdr_len);

    if (pfx_bytes > 0u)
    {
        if (prefix_addr->family == AF_INET)
        {
            const uint8_t *bytes = (const uint8_t *)&prefix_addr->u.v4.s_addr;
            g_byte_array_append(arr, bytes, pfx_bytes);
        }
        else
        {
            g_byte_array_append(arr, prefix_addr->u.v6.s6_addr, pfx_bytes);
        }
    }
    return 0;
}

static void isis_lsp_collect_reach_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const isis_if_cfg_t *if_cfg = (const isis_if_cfg_t *)value;
    isis_lsp_adv_ctx_t *ctx = (isis_lsp_adv_ctx_t *)user_data;
    if (!if_cfg || !ctx || !ctx->inst)
    {
        return;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(if_cfg->ifname);
    if (!isis_if_entry_matches_instance(ctx->inst, if_entry) || !if_entry->proto_up || if_entry->ifindex == 0u)
    {
        return;
    }

    const uint8_t narrow = (ctx->inst->cost_style == ISIS_COST_STYLE_NARROW) ? 1u : 0u;

    const isis_if_af_cfg_t *af_cfg_v4 = isis_lsp_if_af_cfg(ctx->inst, if_cfg, ISIS_AFI_IPV4);
    if (af_cfg_v4 && if_entry->ipv4_addr.family == AF_INET && if_entry->ipv4_prefix_len <= 32u)
    {
        uint32_t metric = (af_cfg_v4->metric == 0u) ? ISIS_DEFAULT_IF_METRIC : af_cfg_v4->metric;
        net_addr_t prefix = if_entry->ipv4_addr;
        if (net_addr_prefix_normalize(&prefix, if_entry->ipv4_prefix_len) == 0)
        {
            if (narrow)
            {
                (void)isis_lsp_append_reach_entry_narrow_v4(ctx->v4_entries, &prefix, if_entry->ipv4_prefix_len,
                                                            metric);
            }
            else
            {
                (void)isis_lsp_append_reach_entry(ctx->v4_entries, &prefix, if_entry->ipv4_prefix_len, metric);
            }
        }
    }

    /* IPv6 仅在 wide cost-style 下发布（narrow/RFC 1195 不支持 IPv6） */
    if (!narrow)
    {
        const isis_if_af_cfg_t *af_cfg_v6 = isis_lsp_if_af_cfg(ctx->inst, if_cfg, ISIS_AFI_IPV6);
        if (af_cfg_v6 && if_entry->ipv6_addr.family == AF_INET6 && if_entry->ipv6_prefix_len <= 128u)
        {
            uint32_t metric = (af_cfg_v6->metric == 0u) ? ISIS_DEFAULT_IF_METRIC : af_cfg_v6->metric;
            net_addr_t prefix = if_entry->ipv6_addr;
            if (net_addr_prefix_normalize(&prefix, if_entry->ipv6_prefix_len) == 0)
            {
                (void)isis_lsp_append_reach_entry(ctx->v6_entries, &prefix, if_entry->ipv6_prefix_len, metric);
            }
        }
    }
}

static void isis_lsp_append_srv6_locator(GByteArray *v6_entries, const isis_srv6_locator_prefix_t *locator)
{
    if (!locator || !v6_entries || locator->prefix.family != AF_INET6 || locator->prefix_len > 127u)
    {
        return;
    }
    /* 本机 locator aggregate 属于本地内部可达前缀；和 loopback 前缀一样以
     * metric 0 进入 RFC 5308 TLV 236。远端 ISIS 路由不会进入此专用缓存。 */
    (void)isis_lsp_append_reach_entry(v6_entries, &locator->prefix, locator->prefix_len, 0u);
}

/** 检查该 (instance, ifname, level) 是否有 UP 邻居（用于判断 LAN 是否上行） */
static int isis_lsp_lan_has_up_adj(const isis_instance_cfg_t *inst, const char *ifname, uint8_t level)
{
    if (!inst || !inst->neighbors || !ifname)
    {
        return 0;
    }
    GHashTableIter iter;
    gpointer k = NULL;
    gpointer v = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, &k, &v))
    {
        (void)k;
        const isis_neighbor_t *nbr = (const isis_neighbor_t *)v;
        if (nbr && nbr->level == level && nbr->state == ISIS_ADJ_STATE_UP && strcmp(nbr->ifname, ifname) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/** 每个有 UP 邻居的 LAN 接口，向 LSP 写一条 IS reach，目标是该 LAN 的 DIS LAN-ID（伪节点）。 */
static void isis_lsp_collect_is_reach_per_if_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const isis_if_cfg_t *if_cfg = (const isis_if_cfg_t *)value;
    isis_lsp_adv_ctx_t *ctx = (isis_lsp_adv_ctx_t *)user_data;
    if (!if_cfg || !ctx || !ctx->inst || !ctx->is_entries)
    {
        return;
    }
    if (!isis_lsp_lan_has_up_adj(ctx->inst, if_cfg->ifname, ctx->level))
    {
        return;
    }
    const isis_if_af_cfg_t *active_af_cfg = isis_lsp_pick_active_af_cfg(ctx->inst, if_cfg);
    if (!active_af_cfg)
    {
        return;
    }
    const isis_dis_state_t *dis = (ctx->level == 1u) ? &if_cfg->dis_l1 : &if_cfg->dis_l2;
    if (dis->lan_id[6] == 0u && !dis->we_are_dis)
    {
        /* DIS 还没收敛，跳过 */
        return;
    }

    /* 目标 LAN-ID：DIS 的 sysid+circuit-id */
    uint8_t target_lan_id[7];
    memcpy(target_lan_id, dis->lan_id, 7u);
    if (dis->we_are_dis && target_lan_id[6] == 0u)
    {
        target_lan_id[6] = dis->our_circuit_id;
    }
    if (target_lan_id[6] == 0u)
    {
        return;
    }

    uint32_t metric = (active_af_cfg->metric == 0u) ? ISIS_DEFAULT_IF_METRIC : active_af_cfg->metric;
    if (metric > 0x00FFFFFFu)
    {
        metric = 0x00FFFFFFu;
    }

    if (ctx->inst->cost_style == ISIS_COST_STYLE_NARROW)
    {
        uint8_t entry[11];
        entry[0] = (uint8_t)(isis_narrow_clamp_metric(metric) & 0x3Fu);
        entry[1] = 0x80u;
        entry[2] = 0x80u;
        entry[3] = 0x80u;
        memcpy(&entry[4], target_lan_id, 6u); /* sysid */
        entry[10] = target_lan_id[6];         /* circuit-id（pseudonode） */
        g_byte_array_append(ctx->is_entries, entry, sizeof(entry));
    }
    else
    {
        uint8_t entry[11];
        memcpy(entry, target_lan_id, 6u);
        entry[6] = target_lan_id[6];
        entry[7] = (uint8_t)((metric >> 16) & 0xFFu);
        entry[8] = (uint8_t)((metric >> 8) & 0xFFu);
        entry[9] = (uint8_t)(metric & 0xFFu);
        entry[10] = 0u;
        g_byte_array_append(ctx->is_entries, entry, sizeof(entry));
    }
}

static int isis_lsp_build_pdu(const isis_instance_cfg_t *inst, uint8_t level, uint32_t seq, uint8_t *pdu,
                              size_t pdu_cap, size_t *pdu_len_out)
{
    if (!inst || !pdu || !pdu_len_out || pdu_cap < ISIS_LSP_HDR_LEN)
    {
        return -1;
    }

    uint8_t system_id[6];
    if (isis_extract_system_id(inst->net, system_id) != 0)
    {
        return -1;
    }

    GByteArray *is_entries = g_byte_array_new();
    GByteArray *v4_entries = g_byte_array_new();
    GByteArray *v6_entries = g_byte_array_new();
    if (!is_entries || !v4_entries || !v6_entries)
    {
        if (is_entries)
        {
            g_byte_array_free(is_entries, TRUE);
        }
        if (v4_entries)
        {
            g_byte_array_free(v4_entries, TRUE);
        }
        if (v6_entries)
        {
            g_byte_array_free(v6_entries, TRUE);
        }
        return -1;
    }

    isis_lsp_adv_ctx_t adv_ctx = {
        .inst = inst,
        .level = level,
        .is_entries = is_entries,
        .v4_entries = v4_entries,
        .v6_entries = v6_entries,
    };
    if (inst->if_cfgs)
    {
        g_hash_table_foreach(inst->if_cfgs, isis_lsp_collect_reach_cb, &adv_ctx);
        /* IS 邻接走 per-interface 聚合，目标是 LAN 的 DIS LAN-ID（伪节点）而非每个邻居 sysid。 */
        g_hash_table_foreach(inst->if_cfgs, isis_lsp_collect_is_reach_per_if_cb, &adv_ctx);
    }
    if (inst->vrf_id == ROUTE_VRF_DEFAULT && inst->af_ipv6 && inst->cost_style != ISIS_COST_STYLE_NARROW &&
        inst->srv6_locator[0] != '\0' && g_isis_work_local && g_isis_work_local->srv6_locators)
    {
        const isis_srv6_locator_prefix_t *locator =
            g_hash_table_lookup(g_isis_work_local->srv6_locators, inst->srv6_locator);
        isis_lsp_append_srv6_locator(v6_entries, locator);
    }

    size_t p = 0;
    pdu[p++] = ISIS_NLPID;
    pdu[p++] = ISIS_LSP_HDR_LEN;
    pdu[p++] = 1u;
    pdu[p++] = 6u;
    pdu[p++] = (level == 1u) ? ISIS_PDU_TYPE_LSP_L1 : ISIS_PDU_TYPE_LSP_L2;
    pdu[p++] = 1u;
    pdu[p++] = 0u;
    pdu[p++] = 3u;

    size_t pdu_len_pos = p;
    pdu[p++] = 0u;
    pdu[p++] = 0u;

    pdu[p++] = (uint8_t)((ISIS_LSP_DEFAULT_LIFETIME_SEC >> 8) & 0xFFu);
    pdu[p++] = (uint8_t)(ISIS_LSP_DEFAULT_LIFETIME_SEC & 0xFFu);

    memcpy(&pdu[p], system_id, 6u);
    p += 6u;
    pdu[p++] = 0u;
    pdu[p++] = 0u;

    pdu[p++] = (uint8_t)((seq >> 24) & 0xFFu);
    pdu[p++] = (uint8_t)((seq >> 16) & 0xFFu);
    pdu[p++] = (uint8_t)((seq >> 8) & 0xFFu);
    pdu[p++] = (uint8_t)(seq & 0xFFu);

    pdu[p++] = 0u;
    pdu[p++] = 0u;

    pdu[p++] = (level == 1u) ? 1u : 2u;

    int rc = 0;
    const uint8_t narrow = (inst->cost_style == ISIS_COST_STYLE_NARROW) ? 1u : 0u;

    /* RFC 1195/ISO 10589 必备元 TLV：Area + Protocols Supported。FRR/IOS-XR 等收到
     * 缺这两个 TLV 的 LSP 会在 SPF 阶段把发起者当成"不能讲 IP"的节点而忽略整个 LSP。 */
    {
        uint8_t area[64];
        uint8_t area_len = 0u;
        if (isis_lsp_extract_area(inst->net, area, sizeof(area), &area_len) == 0 && area_len > 0u)
        {
            uint8_t area_val[1 + 64];
            area_val[0] = area_len;
            memcpy(&area_val[1], area, area_len);
            (void)isis_lsp_append_single_tlv(pdu, pdu_cap, &p, ISIS_TLV_AREA_ADDR, area_val, (uint8_t)(area_len + 1u));
        }

        uint8_t nlpids[2];
        uint8_t nlpids_len = 0u;
        if (inst->af_ipv4)
        {
            nlpids[nlpids_len++] = ISIS_NLPID_IPV4;
        }
        if (inst->af_ipv6 && !narrow)
        {
            nlpids[nlpids_len++] = ISIS_NLPID_IPV6;
        }
        if (nlpids_len > 0u)
        {
            (void)isis_lsp_append_single_tlv(pdu, pdu_cap, &p, ISIS_TLV_PROTOCOLS_SUPPORTED, nlpids, nlpids_len);
        }
    }

    if (is_entries->len > 0u)
    {
        if (narrow)
        {
            rc = isis_lsp_append_tlv2_chunks(pdu, pdu_cap, &p, is_entries->data, is_entries->len);
        }
        else
        {
            rc = isis_lsp_append_tlv_chunks(pdu, pdu_cap, &p, ISIS_TLV_EXT_IS_REACH, is_entries->data, is_entries->len);
        }
    }
    if (rc == 0 && v4_entries->len > 0u)
    {
        uint8_t tlv_type = narrow ? ISIS_TLV_IP_INT_REACH : ISIS_TLV_EXT_IP_REACH;
        rc = isis_lsp_append_tlv_chunks(pdu, pdu_cap, &p, tlv_type, v4_entries->data, v4_entries->len);
    }
    if (rc == 0 && !narrow && v6_entries->len > 0u)
    {
        rc = isis_lsp_append_tlv_chunks(pdu, pdu_cap, &p, ISIS_TLV_IPV6_REACH, v6_entries->data, v6_entries->len);
    }

    g_byte_array_free(is_entries, TRUE);
    g_byte_array_free(v4_entries, TRUE);
    g_byte_array_free(v6_entries, TRUE);

    if (rc != 0 || p > 0xFFFFu)
    {
        return -1;
    }

    pdu[pdu_len_pos] = (uint8_t)((p >> 8) & 0xFFu);
    pdu[pdu_len_pos + 1u] = (uint8_t)(p & 0xFFu);
    if (isis_lsp_checksum_fill(&pdu[ISIS_LSP_CKSUM_COVER_OFFSET], p - ISIS_LSP_CKSUM_COVER_OFFSET,
                               ISIS_LSP_CKSUM_FIELD_REL_OFFSET) != 0)
    {
        return -1;
    }
    *pdu_len_out = p;
    return 0;
}

/* 构造一条伪节点 LSP：LSP-ID = <our_sysid>.<circuit_id>-00；body 包含 Area + Protocols + 一组
 * IS reach（到本 LAN 上所有 UP 邻居 + 自己，metric=0，pseudonode-id=0）。
 * 仅 we_are_dis 的 (level, interface) 才应调用本函数。 */
static int isis_lsp_build_pseudonode_pdu(const isis_instance_cfg_t *inst, uint8_t level, const char *ifname,
                                         uint8_t circuit_id, uint32_t seq, uint8_t *pdu, size_t pdu_cap,
                                         size_t *pdu_len_out)
{
    if (!inst || !ifname || circuit_id == 0u || !pdu || !pdu_len_out || pdu_cap < ISIS_LSP_HDR_LEN)
    {
        return -1;
    }

    uint8_t system_id[6];
    if (isis_extract_system_id(inst->net, system_id) != 0)
    {
        return -1;
    }

    GByteArray *is_entries = g_byte_array_new();
    if (!is_entries)
    {
        return -1;
    }
    const uint8_t narrow = (inst->cost_style == ISIS_COST_STYLE_NARROW) ? 1u : 0u;

    /* 加入本机自己（sysid + pn=0），度量 0 */
    {
        uint8_t entry[11];
        memset(entry, 0, sizeof(entry));
        if (narrow)
        {
            entry[0] = 0u;
            entry[1] = 0x80u;
            entry[2] = 0x80u;
            entry[3] = 0x80u;
            memcpy(&entry[4], system_id, 6u);
            entry[10] = 0u;
        }
        else
        {
            memcpy(entry, system_id, 6u);
            entry[6] = 0u;
            entry[7] = 0u;
            entry[8] = 0u;
            entry[9] = 0u;
            entry[10] = 0u;
        }
        g_byte_array_append(is_entries, entry, sizeof(entry));
    }

    /* 加入该 (ifname, level) 上所有 UP 邻居 */
    if (inst->neighbors)
    {
        GHashTableIter iter;
        gpointer k = NULL;
        gpointer v = NULL;
        g_hash_table_iter_init(&iter, inst->neighbors);
        while (g_hash_table_iter_next(&iter, &k, &v))
        {
            (void)k;
            const isis_neighbor_t *nbr = (const isis_neighbor_t *)v;
            if (!nbr || nbr->level != level || nbr->state != ISIS_ADJ_STATE_UP)
            {
                continue;
            }
            if (strcmp(nbr->ifname, ifname) != 0)
            {
                continue;
            }
            uint8_t entry[11];
            memset(entry, 0, sizeof(entry));
            if (narrow)
            {
                entry[0] = 0u;
                entry[1] = 0x80u;
                entry[2] = 0x80u;
                entry[3] = 0x80u;
                memcpy(&entry[4], nbr->system_id, 6u);
                entry[10] = 0u;
            }
            else
            {
                memcpy(entry, nbr->system_id, 6u);
                entry[6] = 0u;
                entry[7] = 0u;
                entry[8] = 0u;
                entry[9] = 0u;
                entry[10] = 0u;
            }
            g_byte_array_append(is_entries, entry, sizeof(entry));
        }
    }

    /* 组 LSP 头 */
    size_t p = 0;
    pdu[p++] = ISIS_NLPID;
    pdu[p++] = ISIS_LSP_HDR_LEN;
    pdu[p++] = 1u;
    pdu[p++] = 6u;
    pdu[p++] = (level == 1u) ? ISIS_PDU_TYPE_LSP_L1 : ISIS_PDU_TYPE_LSP_L2;
    pdu[p++] = 1u;
    pdu[p++] = 0u;
    pdu[p++] = 3u;

    size_t pdu_len_pos = p;
    pdu[p++] = 0u;
    pdu[p++] = 0u;

    pdu[p++] = (uint8_t)((ISIS_LSP_DEFAULT_LIFETIME_SEC >> 8) & 0xFFu);
    pdu[p++] = (uint8_t)(ISIS_LSP_DEFAULT_LIFETIME_SEC & 0xFFu);

    memcpy(&pdu[p], system_id, 6u);
    p += 6u;
    pdu[p++] = circuit_id; /* pseudonode-id */
    pdu[p++] = 0u;         /* fragment */

    pdu[p++] = (uint8_t)((seq >> 24) & 0xFFu);
    pdu[p++] = (uint8_t)((seq >> 16) & 0xFFu);
    pdu[p++] = (uint8_t)((seq >> 8) & 0xFFu);
    pdu[p++] = (uint8_t)(seq & 0xFFu);

    pdu[p++] = 0u; /* checksum hi */
    pdu[p++] = 0u; /* checksum lo */

    pdu[p++] = (level == 1u) ? 1u : 2u;

    int rc = 0;
    /* Area */
    {
        uint8_t area[64];
        uint8_t area_len = 0u;
        if (isis_lsp_extract_area(inst->net, area, sizeof(area), &area_len) == 0 && area_len > 0u)
        {
            uint8_t area_val[1 + 64];
            area_val[0] = area_len;
            memcpy(&area_val[1], area, area_len);
            rc |= isis_lsp_append_single_tlv(pdu, pdu_cap, &p, ISIS_TLV_AREA_ADDR, area_val, (uint8_t)(area_len + 1u));
        }
        /* Protocols */
        uint8_t nlpids[2];
        uint8_t nlpids_len = 0u;
        if (inst->af_ipv4)
        {
            nlpids[nlpids_len++] = ISIS_NLPID_IPV4;
        }
        if (inst->af_ipv6 && !narrow)
        {
            nlpids[nlpids_len++] = ISIS_NLPID_IPV6;
        }
        if (nlpids_len > 0u)
        {
            rc |= isis_lsp_append_single_tlv(pdu, pdu_cap, &p, ISIS_TLV_PROTOCOLS_SUPPORTED, nlpids, nlpids_len);
        }
    }

    /* IS reach entries */
    if (is_entries->len > 0u)
    {
        if (narrow)
        {
            rc |= isis_lsp_append_tlv2_chunks(pdu, pdu_cap, &p, is_entries->data, is_entries->len);
        }
        else
        {
            rc |=
                isis_lsp_append_tlv_chunks(pdu, pdu_cap, &p, ISIS_TLV_EXT_IS_REACH, is_entries->data, is_entries->len);
        }
    }

    g_byte_array_free(is_entries, TRUE);

    if (rc != 0 || p > 0xFFFFu)
    {
        return -1;
    }

    pdu[pdu_len_pos] = (uint8_t)((p >> 8) & 0xFFu);
    pdu[pdu_len_pos + 1u] = (uint8_t)(p & 0xFFu);
    if (isis_lsp_checksum_fill(&pdu[ISIS_LSP_CKSUM_COVER_OFFSET], p - ISIS_LSP_CKSUM_COVER_OFFSET,
                               ISIS_LSP_CKSUM_FIELD_REL_OFFSET) != 0)
    {
        return -1;
    }
    *pdu_len_out = p;
    return 0;
}

typedef struct isis_lsp_flood_if_ctx
{
    const isis_instance_cfg_t *inst;
    int raw_fd;
    uint8_t level;
    const uint8_t *pdu;
    size_t pdu_len;
    const char *ingress_ifname;
    uint32_t ingress_ifindex;
    uint32_t sent;
} isis_lsp_flood_if_ctx_t;

static void isis_lsp_flood_if_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const isis_if_cfg_t *if_cfg = (const isis_if_cfg_t *)value;
    isis_lsp_flood_if_ctx_t *ctx = (isis_lsp_flood_if_ctx_t *)user_data;
    if (!if_cfg || !ctx || !ctx->pdu || ctx->pdu_len == 0u)
    {
        return;
    }
    if (!isis_lsp_pick_active_af_cfg(ctx->inst, if_cfg))
    {
        return;
    }
    if (ctx->ingress_ifname && strcmp(if_cfg->ifname, ctx->ingress_ifname) == 0)
    {
        return;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(if_cfg->ifname);
    if (!isis_if_entry_matches_instance(ctx->inst, if_entry) || !if_entry->proto_up || if_entry->ifindex == 0u)
    {
        return;
    }
    if (ctx->ingress_ifindex != 0u && if_entry->ifindex == ctx->ingress_ifindex)
    {
        return;
    }
    if (!isis_lsp_if_has_up_adjacency(ctx->inst, if_cfg->ifname, ctx->level))
    {
        return;
    }

    if (isis_lsp_send_pdu_on_if(ctx->raw_fd, if_entry, ctx->level, ctx->pdu, ctx->pdu_len) == 0)
    {
        ctx->sent++;
    }
}

static void isis_lsp_send_if_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const isis_if_cfg_t *if_cfg = (const isis_if_cfg_t *)value;
    isis_lsp_send_if_ctx_t *ctx = (isis_lsp_send_if_ctx_t *)user_data;
    if (!if_cfg || !ctx)
    {
        return;
    }
    if (!isis_lsp_pick_active_af_cfg(ctx->inst, if_cfg))
    {
        return;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(if_cfg->ifname);
    if (!isis_if_entry_matches_instance(ctx->inst, if_entry) || !if_entry->proto_up || if_entry->ifindex == 0u)
    {
        return;
    }

    if (ctx->pdu_l1 && ctx->pdu_l1_len > 0u && isis_lsp_if_has_up_adjacency(ctx->inst, if_cfg->ifname, 1u))
    {
        if (isis_lsp_send_pdu_on_if(ctx->raw_fd, if_entry, 1u, ctx->pdu_l1, ctx->pdu_l1_len) == 0)
        {
            ctx->sent_l1++;
        }
    }
    if (ctx->pdu_l2 && ctx->pdu_l2_len > 0u && isis_lsp_if_has_up_adjacency(ctx->inst, if_cfg->ifname, 2u))
    {
        if (isis_lsp_send_pdu_on_if(ctx->raw_fd, if_entry, 2u, ctx->pdu_l2, ctx->pdu_l2_len) == 0)
        {
            ctx->sent_l2++;
        }
    }
}

void isis_lsp_send_due(isis_instance_cfg_t *inst, int raw_fd, uint64_t now_msec)
{
    if (!inst || raw_fd < 0 || !inst->admin_up || !inst->if_cfgs || g_hash_table_size(inst->if_cfgs) == 0)
    {
        return;
    }

    uint64_t interval_msec = (uint64_t)ISIS_LSP_TX_INTERVAL_SEC * 1000u;
    if (inst->last_lsp_tx_msec != 0u && now_msec >= inst->last_lsp_tx_msec &&
        (now_msec - inst->last_lsp_tx_msec) < interval_msec)
    {
        return;
    }

    uint8_t pdu_l1[ISIS_LSP_PKT_MAX];
    uint8_t pdu_l2[ISIS_LSP_PKT_MAX];
    size_t pdu_l1_len = 0;
    size_t pdu_l2_len = 0;

    uint32_t seq_l1 = inst->lsp_seq_l1 + 1u;
    uint32_t seq_l2 = inst->lsp_seq_l2 + 1u;
    if (seq_l1 == 0u)
    {
        seq_l1 = 1u;
    }
    if (seq_l2 == 0u)
    {
        seq_l2 = 1u;
    }

    if (isis_level_enabled(inst, 1u))
    {
        if (isis_lsp_build_pdu(inst, 1u, seq_l1, pdu_l1, sizeof(pdu_l1), &pdu_l1_len) != 0)
        {
            pdu_l1_len = 0u;
        }
    }
    if (isis_level_enabled(inst, 2u))
    {
        if (isis_lsp_build_pdu(inst, 2u, seq_l2, pdu_l2, sizeof(pdu_l2), &pdu_l2_len) != 0)
        {
            pdu_l2_len = 0u;
        }
    }

    if (pdu_l1_len == 0u && pdu_l2_len == 0u)
    {
        return;
    }

    isis_lsp_send_if_ctx_t send_ctx = {
        .inst = inst,
        .raw_fd = raw_fd,
        .pdu_l1 = (pdu_l1_len > 0u) ? pdu_l1 : NULL,
        .pdu_l1_len = pdu_l1_len,
        .pdu_l2 = (pdu_l2_len > 0u) ? pdu_l2 : NULL,
        .pdu_l2_len = pdu_l2_len,
        .sent_l1 = 0u,
        .sent_l2 = 0u,
    };
    g_hash_table_foreach(inst->if_cfgs, isis_lsp_send_if_cb, &send_ctx);

    if (send_ctx.sent_l1 > 0u)
    {
        inst->lsp_seq_l1 = seq_l1;
    }
    if (send_ctx.sent_l2 > 0u)
    {
        inst->lsp_seq_l2 = seq_l2;
    }

    /* 伪节点 LSP：每个 (interface, level) 我们当 DIS 的，发一份 pseudonode LSP，
     * 沿 LAN 泛洪给所有邻居。 */
    GHashTableIter pn_iter;
    gpointer pn_k = NULL;
    gpointer pn_v = NULL;
    g_hash_table_iter_init(&pn_iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&pn_iter, &pn_k, &pn_v))
    {
        (void)pn_k;
        isis_if_cfg_t *pn_if_cfg = (isis_if_cfg_t *)pn_v;
        if (!pn_if_cfg)
        {
            continue;
        }
        const if_api_cache_entry_t *pn_if_entry = if_api_cache_lookup(pn_if_cfg->ifname);
        if (!isis_if_entry_matches_instance(inst, pn_if_entry) || !pn_if_entry->proto_up || pn_if_entry->ifindex == 0u)
        {
            continue;
        }
        for (uint8_t lvl = 1u; lvl <= 2u; ++lvl)
        {
            if (!isis_level_enabled(inst, lvl))
            {
                continue;
            }
            isis_dis_state_t *dis = (lvl == 1u) ? &pn_if_cfg->dis_l1 : &pn_if_cfg->dis_l2;
            if (!dis->we_are_dis || dis->our_circuit_id == 0u)
            {
                continue;
            }
            if (!isis_lsp_if_has_up_adjacency(inst, pn_if_cfg->ifname, lvl))
            {
                /* 没邻居就不发伪节点（避免污染对端 LSDB） */
                continue;
            }
            uint32_t pn_seq = dis->pseudo_seq + 1u;
            if (pn_seq == 0u)
            {
                pn_seq = 1u;
            }
            uint8_t pn_pdu[ISIS_LSP_PKT_MAX];
            size_t pn_pdu_len = 0u;
            if (isis_lsp_build_pseudonode_pdu(inst, lvl, pn_if_cfg->ifname, dis->our_circuit_id, pn_seq, pn_pdu,
                                              sizeof(pn_pdu), &pn_pdu_len) != 0 ||
                pn_pdu_len == 0u)
            {
                continue;
            }
            isis_lsp_flood_if_ctx_t pn_flood_ctx = {
                .inst = inst,
                .raw_fd = raw_fd,
                .level = lvl,
                .pdu = pn_pdu,
                .pdu_len = pn_pdu_len,
                .ingress_ifname = NULL,
                .ingress_ifindex = 0u,
                .sent = 0u,
            };
            g_hash_table_foreach(inst->if_cfgs, isis_lsp_flood_if_cb, &pn_flood_ctx);
            if (pn_flood_ctx.sent > 0u)
            {
                dis->pseudo_seq = pn_seq;
            }
        }
    }

    if (send_ctx.sent_l1 > 0u || send_ctx.sent_l2 > 0u)
    {
        inst->last_lsp_tx_msec = now_msec;
    }
}

static isis_neighbor_t *isis_lsp_find_neighbor(isis_instance_cfg_t *inst, const char *ifname, uint8_t level,
                                               const uint8_t system_id[6])
{
    if (!inst || !inst->neighbors || !ifname || ifname[0] == '\0' || !system_id)
    {
        return NULL;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        isis_neighbor_t *nbr = (isis_neighbor_t *)value;
        if (!nbr)
        {
            continue;
        }
        if (nbr->level == level && strcmp(nbr->ifname, ifname) == 0 && memcmp(nbr->system_id, system_id, 6u) == 0)
        {
            return nbr;
        }
    }
    return NULL;
}

static isis_neighbor_t *isis_lsp_find_sender_neighbor(isis_instance_cfg_t *inst, const char *ifname, uint8_t level,
                                                      const uint8_t src_mac[ETH_ALEN])
{
    if (!inst || !inst->neighbors || !ifname || ifname[0] == '\0' || isis_lsp_mac_is_zero(src_mac))
    {
        return NULL;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        isis_neighbor_t *nbr = (isis_neighbor_t *)value;
        if (!nbr)
        {
            continue;
        }
        if (nbr->level == level && strcmp(nbr->ifname, ifname) == 0 && isis_lsp_mac_equal(nbr->remote_snpa, src_mac))
        {
            return nbr;
        }
    }
    return NULL;
}

static void isis_lsp_flood_instance(isis_instance_cfg_t *inst, const isis_lsp_rx_ctx_t *ctx)
{
    if (!inst || !ctx || ctx->raw_fd < 0 || !ctx->pdu || ctx->pdu_len == 0u || !inst->if_cfgs || !inst->admin_up ||
        !isis_level_enabled(inst, ctx->level))
    {
        return;
    }

    isis_lsp_flood_if_ctx_t flood_ctx = {
        .inst = inst,
        .raw_fd = ctx->raw_fd,
        .level = ctx->level,
        .pdu = ctx->pdu,
        .pdu_len = ctx->pdu_len,
        .ingress_ifname = ctx->ifname,
        .ingress_ifindex = ctx->if_entry ? ctx->if_entry->ifindex : 0u,
    };
    g_hash_table_foreach(inst->if_cfgs, isis_lsp_flood_if_cb, &flood_ctx);
}

static void isis_lsp_apply_instance_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    isis_instance_cfg_t *inst = (isis_instance_cfg_t *)value;
    isis_lsp_rx_ctx_t *ctx = (isis_lsp_rx_ctx_t *)user_data;
    if (!inst || !ctx || !ctx->ifname || !ctx->if_entry || !inst->if_cfgs || !inst->learned_route_heads)
    {
        return;
    }

    if (!inst->admin_up || !isis_level_enabled(inst, ctx->level) ||
        !isis_if_entry_matches_instance(inst, ctx->if_entry))
    {
        return;
    }

    const isis_if_cfg_t *if_cfg = (const isis_if_cfg_t *)g_hash_table_lookup(inst->if_cfgs, ctx->ifname);
    if (!if_cfg || !isis_lsp_pick_active_af_cfg(inst, if_cfg))
    {
        return;
    }

    uint8_t local_sysid[6];
    if (isis_extract_system_id(inst->net, local_sysid) == 0 && memcmp(local_sysid, ctx->system_id, 6u) == 0)
    {
        return;
    }

    isis_neighbor_t *sender_nbr = isis_lsp_find_sender_neighbor(inst, ctx->ifname, ctx->level, ctx->src_mac);
    if (!sender_nbr || sender_nbr->state != ISIS_ADJ_STATE_UP)
    {
        return;
    }

    isis_neighbor_t *origin_nbr = isis_lsp_find_neighbor(inst, ctx->ifname, ctx->level, ctx->system_id);

    isis_lsdb_entry_t *lsdb = NULL;
    char lsdb_key[32] = {0};
    if (inst->lsdb_entries)
    {
        isis_lsp_lsdb_key_format(lsdb_key, sizeof(lsdb_key), ctx->level, ctx->system_id, ctx->pseudonode_id,
                                 ctx->fragment_id);
        lsdb = (isis_lsdb_entry_t *)g_hash_table_lookup(inst->lsdb_entries, lsdb_key);
    }

    /*
     * Sequence numbers are scoped to a single LSP-ID.  A router LSP
     * (<sysid>.00-00) and that router's pseudonode LSPs (<sysid>.<pn>-00)
     * each have independent sequence spaces.  Do not fall back to the
     * neighbor-level last_lsp_seq here, or a fresh pseudonode LSP with seq 1
     * can be discarded after a higher-sequence router LSP from the same
     * system has already been accepted.
     */
    uint32_t last_seq = lsdb ? lsdb->seq : 0u;
    uint64_t last_rx_msec = lsdb ? lsdb->last_rx_msec : 0u;
    if (last_seq != 0u && ctx->seq != 0u && ctx->seq < last_seq)
    {
        if (!(last_rx_msec != 0u && ctx->now_msec >= last_rx_msec &&
              (ctx->now_msec - last_rx_msec) > (uint64_t)ISIS_LSP_SEQ_RESTART_GRACE_MSEC))
        {
            return;
        }
    }
    else if (last_seq != 0u && ctx->seq != 0u && ctx->seq == last_seq && ctx->lifetime_sec != 0u)
    {
        return;
    }

    if (origin_nbr && ctx->pseudonode_id == 0u && ctx->fragment_id == 0u)
    {
        origin_nbr->last_lsp_seq = ctx->seq;
        origin_nbr->last_lsp_rx_msec = ctx->now_msec;
    }

    if (ctx->lifetime_sec == 0u)
    {
        if (inst->lsdb_entries)
        {
            (void)g_hash_table_remove(inst->lsdb_entries, lsdb_key);
        }
        isis_spf_withdraw_origin_routes(inst, ctx->level, ctx->system_id);
        isis_lsp_flood_instance(inst, ctx);
        return;
    }

    if (inst->lsdb_entries)
    {
        if (!lsdb)
        {
            lsdb = g_malloc0(sizeof(*lsdb));
            if (lsdb)
            {
                lsdb->level = ctx->level;
                memcpy(lsdb->system_id, ctx->system_id, sizeof(lsdb->system_id));
                lsdb->pseudonode_id = ctx->pseudonode_id;
                lsdb->fragment_id = ctx->fragment_id;
                g_hash_table_replace(inst->lsdb_entries, g_strdup(lsdb_key), lsdb);
            }
        }

        if (lsdb)
        {
            uint32_t v4_cnt = 0u;
            uint32_t v6_cnt = 0u;
            isis_lsp_count_reach_prefixes(ctx->tlvs, ctx->tlv_len, &v4_cnt, &v6_cnt);

            g_strlcpy(lsdb->rx_ifname, ctx->ifname, sizeof(lsdb->rx_ifname));
            lsdb->seq = ctx->seq;
            lsdb->lifetime_sec = ctx->lifetime_sec;
            lsdb->checksum = ctx->checksum;
            lsdb->ipv4_prefix_count = v4_cnt;
            lsdb->ipv6_prefix_count = v6_cnt;
            lsdb->last_rx_msec = ctx->now_msec;

            if (!lsdb->tlvs)
            {
                lsdb->tlvs = g_byte_array_new();
            }
            if (lsdb->tlvs)
            {
                g_byte_array_set_size(lsdb->tlvs, 0u);
                if (ctx->tlv_len > 0u)
                {
                    g_byte_array_append(lsdb->tlvs, ctx->tlvs, (guint)ctx->tlv_len);
                }
            }
        }
    }

    isis_spf_process_lsp(inst, ctx->level, ctx->system_id, NULL, if_cfg, ctx->if_entry, ctx->tlvs, ctx->tlv_len);

    isis_lsp_flood_instance(inst, ctx);
}

void isis_lsp_handle_pdu(int raw_fd, const uint8_t *pdu, size_t pdu_len, const struct sockaddr_ll *sll,
                         const uint8_t src_mac[ETH_ALEN])
{
    if (!pdu || !sll || pdu_len < ISIS_LSP_HDR_LEN)
    {
        return;
    }
    if (!g_isis_work_local || !g_isis_work_local->instances)
    {
        return;
    }
    if (pdu[0] != ISIS_NLPID)
    {
        return;
    }

    uint8_t pdu_type = pdu[4];
    uint8_t level = 0u;
    if (pdu_type == ISIS_PDU_TYPE_LSP_L1)
    {
        level = 1u;
    }
    else if (pdu_type == ISIS_PDU_TYPE_LSP_L2)
    {
        level = 2u;
    }
    else
    {
        return;
    }

    if ((pdu[3] != 0u && pdu[3] != 6u) || pdu[1] < ISIS_LSP_HDR_LEN)
    {
        return;
    }

    uint16_t declared_len = (uint16_t)(((uint16_t)pdu[8] << 8) | pdu[9]);
    if (declared_len >= ISIS_LSP_HDR_LEN && declared_len <= pdu_len)
    {
        pdu_len = declared_len;
    }
    if (pdu_len < ISIS_LSP_HDR_LEN)
    {
        return;
    }

    uint32_t ifindex = (sll->sll_ifindex > 0) ? (uint32_t)sll->sll_ifindex : 0u;
    const char *ifname = if_api_cache_get_logical_name(ifindex);
    if (!ifname || ifname[0] == '\0')
    {
        return;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(ifname);
    if (!if_entry || !if_entry->proto_up || if_entry->ifindex == 0u)
    {
        return;
    }

    isis_lsp_rx_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.raw_fd = raw_fd;
    ctx.ifname = ifname;
    ctx.if_entry = if_entry;
    ctx.pdu = pdu;
    ctx.pdu_len = pdu_len;
    ctx.tlvs = pdu + ISIS_LSP_HDR_LEN;
    ctx.tlv_len = pdu_len - ISIS_LSP_HDR_LEN;
    ctx.level = level;
    memcpy(ctx.system_id, &pdu[12], sizeof(ctx.system_id));
    ctx.pseudonode_id = pdu[18];
    ctx.fragment_id = pdu[19];
    if (src_mac && !isis_lsp_mac_is_zero(src_mac))
    {
        memcpy(ctx.src_mac, src_mac, sizeof(ctx.src_mac));
    }
    else if (sll->sll_halen >= ETH_ALEN)
    {
        memcpy(ctx.src_mac, sll->sll_addr, sizeof(ctx.src_mac));
    }
    ctx.lifetime_sec = (uint16_t)(((uint16_t)pdu[10] << 8) | pdu[11]);
    if (!isis_lsp_checksum_valid(pdu, pdu_len, ctx.lifetime_sec))
    {
        return;
    }
    ctx.checksum = (uint16_t)(((uint16_t)pdu[24] << 8) | pdu[25]);
    ctx.seq = ((uint32_t)pdu[20] << 24) | ((uint32_t)pdu[21] << 16) | ((uint32_t)pdu[22] << 8) | (uint32_t)pdu[23];
    ctx.now_msec = isis_lsp_now_msec();

    g_hash_table_foreach(g_isis_work_local->instances, isis_lsp_apply_instance_cb, &ctx);
}

void isis_lsp_reconcile_instance(isis_instance_cfg_t *inst, uint64_t now_msec)
{
    if (!inst || !inst->lsdb_entries)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb_entries);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        const isis_lsdb_entry_t *entry = (const isis_lsdb_entry_t *)value;
        if (!entry)
        {
            continue;
        }

        uint32_t life_sec = (entry->lifetime_sec == 0u) ? ISIS_LSP_DEFAULT_LIFETIME_SEC : entry->lifetime_sec;
        uint64_t life_msec = (uint64_t)life_sec * 1000u;
        if (entry->last_rx_msec == 0u)
        {
            continue;
        }
        if (now_msec >= entry->last_rx_msec && (now_msec - entry->last_rx_msec) > life_msec)
        {
            isis_spf_withdraw_origin_routes(inst, entry->level, entry->system_id);
            g_hash_table_iter_remove(&iter);
        }
    }
}

void isis_lsp_remove_origin(isis_instance_cfg_t *inst, uint8_t level, const uint8_t system_id[6])
{
    if (!inst || !inst->lsdb_entries || !system_id)
    {
        return;
    }

    char key[32] = {0};
    isis_lsp_lsdb_key_format(key, sizeof(key), level, system_id, 0u, 0u);
    (void)g_hash_table_remove(inst->lsdb_entries, key);
}
