/**
 * @file   isis_neighbor.c
 * @brief  ISIS LAN IIH 邻居发现、发送与老化
 * @author jhb
 * @date   2026/04/12
 */
#include "isis_neighbor.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "errcode.h"
#include "if_api.h"
#include "isis.h"
#include "isis_lsp.h"
#include "isis_route_sync.h"
#include "isis_spf.h"
#include "log.h"
#include "route.h"

#define ISIS_NEIGHBOR_PKT_MAX 2048u
#define ISIS_NEIGHBOR_KEY_MAX (IF_LOGICAL_NAME_MAX + 32u)
#define ISIS_LEARNED_ROUTE_KEY_MAX (ISIS_NEIGHBOR_KEY_MAX + 24u)
#define ISIS_DEFAULT_HOLD_TIME_SEC 30u
#define ISIS_HELLO_TICK_SEC 1u
#define ISIS_NEIGHBOR_ROUTE_COST 10u

#define ISIS_NLPID 0x83u
#define ISIS_LLC_DSAP 0xFEu
#define ISIS_LLC_SSAP 0xFEu
#define ISIS_LLC_CTRL 0x03u

#define ISIS_PDU_TYPE_LAN_IIH_L1 15u
#define ISIS_PDU_TYPE_LAN_IIH_L2 16u
#define ISIS_LAN_IIH_HDR_LEN 27u

#define ISIS_TLV_AREA_ADDR 1u
#define ISIS_TLV_IS_NEIGHBORS 6u
#define ISIS_TLV_PROTOCOLS_SUPPORTED 129u
#define ISIS_TLV_IPV4_INTF_ADDR 132u
#define ISIS_TLV_IPV6_INTF_ADDR 232u

#define ISIS_NLPID_IPV4 0xCCu
#define ISIS_NLPID_IPV6 0x8Eu

static const uint8_t g_isis_l1_dst_mac[ETH_ALEN] = {0x01u, 0x80u, 0xC2u, 0x00u, 0x00u, 0x14u};
static const uint8_t g_isis_l2_dst_mac[ETH_ALEN] = {0x01u, 0x80u, 0xC2u, 0x00u, 0x00u, 0x15u};

typedef struct isis_neighbor_local
{
    int raw_fd;
    int tick_fd;
    void *raw_tag;
    void *tick_tag;
} isis_neighbor_local_t;

static isis_neighbor_local_t g_isis_neighbor_local = {
    .raw_fd = -1,
    .tick_fd = -1,
    .raw_tag = NULL,
    .tick_tag = NULL,
};

static uint64_t isis_now_msec(void)
{
    return (uint64_t)(g_get_monotonic_time() / 1000);
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

static int isis_instance_af_enabled(const isis_instance_cfg_t *inst, uint16_t afi)
{
    if (!inst)
    {
        return 0;
    }
    if (afi == ROUTE_AFI_IPV4)
    {
        return inst->af_ipv4 ? 1 : 0;
    }
    if (afi == ROUTE_AFI_IPV6)
    {
        return inst->af_ipv6 ? 1 : 0;
    }
    return 0;
}

static const isis_if_af_cfg_t *isis_neighbor_if_af_cfg(const isis_instance_cfg_t *inst, const isis_if_cfg_t *if_cfg,
                                                       uint16_t afi)
{
    if (!inst || !if_cfg || !isis_instance_af_enabled(inst, afi))
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

static const isis_if_af_cfg_t *isis_neighbor_pick_tx_af_cfg(const isis_instance_cfg_t *inst,
                                                            const isis_if_cfg_t *if_cfg)
{
    const isis_if_af_cfg_t *af_cfg_v4 = isis_neighbor_if_af_cfg(inst, if_cfg, ROUTE_AFI_IPV4);
    if (af_cfg_v4 && !af_cfg_v4->passive)
    {
        return af_cfg_v4;
    }

    const isis_if_af_cfg_t *af_cfg_v6 = isis_neighbor_if_af_cfg(inst, if_cfg, ROUTE_AFI_IPV6);
    if (af_cfg_v6 && !af_cfg_v6->passive)
    {
        return af_cfg_v6;
    }

    return NULL;
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

static void isis_neighbor_key_format(char *buf, size_t sz, const char *ifname, uint8_t level, const uint8_t sysid[6])
{
    if (!buf || sz == 0)
    {
        return;
    }
    char sysid_hex[13] = {0};
    isis_sysid_to_hex(sysid, sysid_hex, sizeof(sysid_hex));
    g_snprintf(buf, sz, "%s|%u|%s", ifname ? ifname : "", (unsigned)level, sysid_hex);
}

static void isis_learned_route_key_format(char *buf, size_t sz, const char *ifname, uint8_t level,
                                          const uint8_t sysid[6], uint16_t afi)
{
    if (!buf || sz == 0)
    {
        return;
    }
    char nbr_key[ISIS_NEIGHBOR_KEY_MAX] = {0};
    isis_neighbor_key_format(nbr_key, sizeof(nbr_key), ifname, level, sysid);
    g_snprintf(buf, sz, "host|%s|%u", nbr_key, (unsigned)afi);
}

static void isis_zero_addr(sa_family_t family, net_addr_t *out)
{
    if (!out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->family = family;
}

static int isis_route_state_same(const isis_route_state_t *a, const isis_route_state_t *b)
{
    if (!a || !b)
    {
        return 0;
    }

    return (a->afi == b->afi && a->prefix_len == b->prefix_len && a->out_ifindex == b->out_ifindex &&
            a->metric == b->metric && net_addr_equal(&a->prefix_addr, &b->prefix_addr) &&
            net_addr_equal(&a->source_addr, &b->source_addr) && net_addr_equal(&a->nexthop_addr, &b->nexthop_addr))
               ? 1
               : 0;
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

static int isis_extract_area(const char *net, uint8_t *area, size_t area_cap, uint8_t *area_len)
{
    uint8_t bytes[64];
    size_t len = 0;
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

static int isis_tlv_append(uint8_t *buf, size_t cap, size_t *len_io, uint8_t type, const void *val, uint8_t val_len)
{
    if (!buf || !len_io)
    {
        return -1;
    }
    size_t len = *len_io;
    if (len + 2u + (size_t)val_len > cap)
    {
        return -1;
    }

    buf[len++] = type;
    buf[len++] = val_len;
    if (val_len > 0u && val)
    {
        memcpy(&buf[len], val, val_len);
        len += val_len;
    }
    *len_io = len;
    return 0;
}

static int isis_build_iih_pdu(const isis_instance_cfg_t *inst, const if_api_cache_entry_t *if_entry, uint8_t level,
                              const uint8_t system_id[6], uint16_t hold_time_sec, uint8_t *pdu, size_t pdu_cap,
                              size_t *pdu_len_out)
{
    if (!inst || !if_entry || !system_id || !pdu || !pdu_len_out || pdu_cap < ISIS_LAN_IIH_HDR_LEN)
    {
        return -1;
    }

    size_t p = 0;
    pdu[p++] = ISIS_NLPID;
    pdu[p++] = ISIS_LAN_IIH_HDR_LEN;
    pdu[p++] = 1u;
    pdu[p++] = 6u;
    pdu[p++] = (level == 1u) ? ISIS_PDU_TYPE_LAN_IIH_L1 : ISIS_PDU_TYPE_LAN_IIH_L2;
    pdu[p++] = 1u;
    pdu[p++] = 0u;
    pdu[p++] = 3u;

    uint8_t circuit_type = 3u;
    if (inst->is_type == ISIS_IS_TYPE_LEVEL_1)
    {
        circuit_type = 1u;
    }
    else if (inst->is_type == ISIS_IS_TYPE_LEVEL_2)
    {
        circuit_type = 2u;
    }
    pdu[p++] = circuit_type;

    memcpy(&pdu[p], system_id, 6u);
    p += 6u;

    pdu[p++] = (uint8_t)((hold_time_sec >> 8) & 0xFFu);
    pdu[p++] = (uint8_t)(hold_time_sec & 0xFFu);

    size_t pdu_len_pos = p;
    pdu[p++] = 0u;
    pdu[p++] = 0u;

    pdu[p++] = 64u;
    memcpy(&pdu[p], system_id, 6u);
    p += 6u;
    pdu[p++] = 0u;

    uint8_t area[64];
    uint8_t area_len = 0u;
    if (isis_extract_area(inst->net, area, sizeof(area), &area_len) == 0 && area_len > 0u)
    {
        uint8_t area_val[65];
        area_val[0] = area_len;
        memcpy(&area_val[1], area, area_len);
        if (isis_tlv_append(pdu, pdu_cap, &p, ISIS_TLV_AREA_ADDR, area_val, (uint8_t)(area_len + 1u)) != 0)
        {
            return -1;
        }
    }

    uint8_t nlpids[2];
    uint8_t nlpids_len = 0u;
    if (inst->af_ipv4)
    {
        nlpids[nlpids_len++] = ISIS_NLPID_IPV4;
    }
    if (inst->af_ipv6)
    {
        nlpids[nlpids_len++] = ISIS_NLPID_IPV6;
    }
    if (nlpids_len > 0u)
    {
        if (isis_tlv_append(pdu, pdu_cap, &p, ISIS_TLV_PROTOCOLS_SUPPORTED, nlpids, nlpids_len) != 0)
        {
            return -1;
        }
    }

    if (inst->af_ipv4 && if_entry->ipv4_addr.family == AF_INET)
    {
        if (isis_tlv_append(pdu, pdu_cap, &p, ISIS_TLV_IPV4_INTF_ADDR, &if_entry->ipv4_addr.u.v4, 4u) != 0)
        {
            return -1;
        }
    }
    if (inst->af_ipv6 && if_entry->ipv6_addr.family == AF_INET6)
    {
        if (isis_tlv_append(pdu, pdu_cap, &p, ISIS_TLV_IPV6_INTF_ADDR, &if_entry->ipv6_addr.u.v6, 16u) != 0)
        {
            return -1;
        }
    }

    if (isis_tlv_append(pdu, pdu_cap, &p, ISIS_TLV_IS_NEIGHBORS, NULL, 0u) != 0)
    {
        return -1;
    }

    if (p > 0xFFFFu)
    {
        return -1;
    }
    pdu[pdu_len_pos] = (uint8_t)((p >> 8) & 0xFFu);
    pdu[pdu_len_pos + 1u] = (uint8_t)(p & 0xFFu);

    *pdu_len_out = p;
    return 0;
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

static void isis_send_iih_on_if(isis_instance_cfg_t *inst, isis_if_cfg_t *if_cfg, const if_api_cache_entry_t *if_entry,
                                uint8_t level)
{
    if (!inst || !if_cfg || !if_entry || g_isis_neighbor_local.raw_fd < 0 || if_entry->ifindex == 0u)
    {
        return;
    }

    uint8_t system_id[6];
    if (isis_extract_system_id(inst->net, system_id) != 0)
    {
        return;
    }

    const isis_if_af_cfg_t *tx_af_cfg = isis_neighbor_pick_tx_af_cfg(inst, if_cfg);
    if (!tx_af_cfg)
    {
        return;
    }

    uint16_t hello_interval =
        (tx_af_cfg->hello_interval == 0u) ? ISIS_DEFAULT_HELLO_INTERVAL : tx_af_cfg->hello_interval;
    uint16_t hold_mult = (tx_af_cfg->hold_multiplier == 0u) ? ISIS_DEFAULT_HOLD_MULTIPLIER : tx_af_cfg->hold_multiplier;
    uint16_t hold_time_sec = (uint16_t)(hello_interval * hold_mult);
    if (hold_time_sec == 0u)
    {
        hold_time_sec = ISIS_DEFAULT_HOLD_TIME_SEC;
    }

    uint8_t pdu[256];
    size_t pdu_len = 0;
    if (isis_build_iih_pdu(inst, if_entry, level, system_id, hold_time_sec, pdu, sizeof(pdu), &pdu_len) != 0)
    {
        return;
    }

    uint8_t src_mac[ETH_ALEN];
    if (isis_get_src_mac(g_isis_neighbor_local.raw_fd, if_entry, src_mac) != 0)
    {
        return;
    }

    const uint8_t *dst_mac = (level == 1u) ? g_isis_l1_dst_mac : g_isis_l2_dst_mac;

    uint8_t frame[512];
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

    if (sendto(g_isis_neighbor_local.raw_fd, frame, f, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_WARN("ISIS: send IIH failed on %s(ifindex=%u): %s", if_cfg->ifname, if_entry->ifindex, strerror(errno));
        }
    }
}

typedef struct isis_hello_if_ctx
{
    isis_instance_cfg_t *inst;
    uint64_t now_msec;
} isis_hello_if_ctx_t;

static void isis_send_hello_if_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    isis_if_cfg_t *if_cfg = (isis_if_cfg_t *)value;
    isis_hello_if_ctx_t *ctx = (isis_hello_if_ctx_t *)user_data;
    if (!if_cfg || !ctx || !ctx->inst)
    {
        return;
    }

    const isis_if_af_cfg_t *tx_af_cfg = isis_neighbor_pick_tx_af_cfg(ctx->inst, if_cfg);
    if (!tx_af_cfg)
    {
        return;
    }

    uint16_t hello_interval =
        (tx_af_cfg->hello_interval == 0u) ? ISIS_DEFAULT_HELLO_INTERVAL : tx_af_cfg->hello_interval;
    uint64_t interval_msec = (uint64_t)hello_interval * 1000u;
    if (if_cfg->last_hello_tx_msec != 0u && ctx->now_msec >= if_cfg->last_hello_tx_msec &&
        (ctx->now_msec - if_cfg->last_hello_tx_msec) < interval_msec)
    {
        return;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(if_cfg->ifname);
    if (!if_entry || !if_entry->admin_up || if_entry->ifindex == 0u)
    {
        return;
    }

    if (isis_level_enabled(ctx->inst, 1u))
    {
        isis_send_iih_on_if(ctx->inst, if_cfg, if_entry, 1u);
    }
    if (isis_level_enabled(ctx->inst, 2u))
    {
        isis_send_iih_on_if(ctx->inst, if_cfg, if_entry, 2u);
    }

    if_cfg->last_hello_tx_msec = ctx->now_msec;
}

static void isis_send_hello_instance(isis_instance_cfg_t *inst, uint64_t now_msec)
{
    if (!inst || !inst->admin_up || !inst->if_cfgs)
    {
        return;
    }

    isis_hello_if_ctx_t ctx = {
        .inst = inst,
        .now_msec = now_msec,
    };
    g_hash_table_foreach(inst->if_cfgs, isis_send_hello_if_cb, &ctx);
}

static void isis_neighbor_withdraw_learned_afi(isis_instance_cfg_t *inst, const isis_neighbor_t *nbr, uint16_t afi)
{
    if (!inst || !inst->learned_routes || !nbr)
    {
        return;
    }

    char key_buf[ISIS_LEARNED_ROUTE_KEY_MAX];
    isis_learned_route_key_format(key_buf, sizeof(key_buf), nbr->ifname, nbr->level, nbr->system_id, afi);

    isis_route_state_t *state = (isis_route_state_t *)g_hash_table_lookup(inst->learned_routes, key_buf);
    if (!state)
    {
        return;
    }

    (void)isis_route_sync_publish_del(state);
    (void)g_hash_table_remove(inst->learned_routes, key_buf);
}

static void isis_neighbor_withdraw_learned_all(isis_instance_cfg_t *inst, const isis_neighbor_t *nbr)
{
    isis_neighbor_withdraw_learned_afi(inst, nbr, ROUTE_AFI_IPV4);
    isis_neighbor_withdraw_learned_afi(inst, nbr, ROUTE_AFI_IPV6);
}

static void isis_neighbor_reconcile_learned_afi(isis_instance_cfg_t *inst, const isis_neighbor_t *nbr,
                                                const isis_if_cfg_t *if_cfg, const if_api_cache_entry_t *if_entry,
                                                uint16_t afi)
{
    if (!inst || !nbr || !inst->learned_routes)
    {
        return;
    }

    char key_buf[ISIS_LEARNED_ROUTE_KEY_MAX];
    isis_learned_route_key_format(key_buf, sizeof(key_buf), nbr->ifname, nbr->level, nbr->system_id, afi);
    isis_route_state_t *current = (isis_route_state_t *)g_hash_table_lookup(inst->learned_routes, key_buf);

    int af_enabled = (afi == ROUTE_AFI_IPV4) ? (inst->af_ipv4 != 0u) : (inst->af_ipv6 != 0u);
    const isis_if_af_cfg_t *af_cfg = isis_neighbor_if_af_cfg(inst, if_cfg, afi);
    int has_desired = 0;
    isis_route_state_t desired;
    memset(&desired, 0, sizeof(desired));

    if (inst->admin_up && af_enabled && af_cfg && if_entry && if_entry->admin_up && if_entry->ifindex != 0u &&
        !af_cfg->passive && isis_level_enabled(inst, nbr->level))
    {
        desired.afi = afi;
        desired.out_ifindex = if_entry->ifindex;
        desired.metric = ((af_cfg->metric == 0u) ? ISIS_DEFAULT_IF_METRIC : af_cfg->metric) + ISIS_NEIGHBOR_ROUTE_COST;

        if (afi == ROUTE_AFI_IPV4 && nbr->ipv4_addr.family == AF_INET)
        {
            desired.prefix_len = 32u;
            desired.prefix_addr = nbr->ipv4_addr;
            isis_zero_addr(AF_INET, &desired.nexthop_addr);
            if (if_entry->ipv4_addr.family == AF_INET)
            {
                desired.source_addr = if_entry->ipv4_addr;
            }
            else
            {
                isis_zero_addr(AF_INET, &desired.source_addr);
            }

            if (!(if_entry->ipv4_addr.family == AF_INET && net_addr_equal(&if_entry->ipv4_addr, &nbr->ipv4_addr)))
            {
                has_desired = 1;
            }
        }
        else if (afi == ROUTE_AFI_IPV6 && nbr->ipv6_addr.family == AF_INET6)
        {
            desired.prefix_len = 128u;
            desired.prefix_addr = nbr->ipv6_addr;
            isis_zero_addr(AF_INET6, &desired.nexthop_addr);
            if (if_entry->ipv6_addr.family == AF_INET6)
            {
                desired.source_addr = if_entry->ipv6_addr;
            }
            else
            {
                isis_zero_addr(AF_INET6, &desired.source_addr);
            }

            if (!(if_entry->ipv6_addr.family == AF_INET6 && net_addr_equal(&if_entry->ipv6_addr, &nbr->ipv6_addr)))
            {
                has_desired = 1;
            }
        }
    }

    if (!has_desired)
    {
        if (current)
        {
            (void)isis_route_sync_publish_del(current);
            (void)g_hash_table_remove(inst->learned_routes, key_buf);
        }
        return;
    }

    if (current && isis_route_state_same(current, &desired))
    {
        return;
    }

    if (current)
    {
        (void)isis_route_sync_publish_del(current);
        (void)g_hash_table_remove(inst->learned_routes, key_buf);
    }

    isis_route_state_t *next = g_malloc0(sizeof(*next));
    if (!next)
    {
        return;
    }
    *next = desired;

    if (isis_route_sync_publish_add(next) != ERRCODE_SUCCESS)
    {
        g_free(next);
        return;
    }
    g_hash_table_replace(inst->learned_routes, g_strdup(key_buf), next);
}

static void isis_neighbor_reconcile_learned(isis_instance_cfg_t *inst, const isis_neighbor_t *nbr)
{
    if (!inst || !nbr)
    {
        return;
    }

    const isis_if_cfg_t *if_cfg = NULL;
    if (inst->if_cfgs)
    {
        if_cfg = (const isis_if_cfg_t *)g_hash_table_lookup(inst->if_cfgs, nbr->ifname);
    }
    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(nbr->ifname);

    isis_neighbor_reconcile_learned_afi(inst, nbr, if_cfg, if_entry, ROUTE_AFI_IPV4);
    isis_neighbor_reconcile_learned_afi(inst, nbr, if_cfg, if_entry, ROUTE_AFI_IPV6);
}

static int isis_neighbor_should_remove(const isis_instance_cfg_t *inst, const isis_neighbor_t *nbr, uint64_t now_msec,
                                       const char *if_filter)
{
    if (!inst || !nbr)
    {
        return 1;
    }
    if (if_filter && if_filter[0] != '\0' && strcmp(nbr->ifname, if_filter) != 0)
    {
        return 0;
    }

    if (!inst->admin_up || !isis_level_enabled(inst, nbr->level))
    {
        return 1;
    }

    const isis_if_cfg_t *if_cfg = NULL;
    if (inst->if_cfgs)
    {
        if_cfg = (const isis_if_cfg_t *)g_hash_table_lookup(inst->if_cfgs, nbr->ifname);
    }
    if (!if_cfg || !isis_neighbor_pick_tx_af_cfg(inst, if_cfg))
    {
        return 1;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(nbr->ifname);
    if (!if_entry || !if_entry->admin_up || if_entry->ifindex == 0u)
    {
        return 1;
    }

    uint64_t hold_msec =
        (uint64_t)((nbr->hold_time_sec == 0u) ? ISIS_DEFAULT_HOLD_TIME_SEC : nbr->hold_time_sec) * 1000u;
    if (nbr->last_seen_msec == 0u)
    {
        return 1;
    }
    if (now_msec >= nbr->last_seen_msec && (now_msec - nbr->last_seen_msec) > hold_msec)
    {
        return 1;
    }
    return 0;
}

static void isis_neighbor_reconcile_instance_now(isis_instance_cfg_t *inst, uint64_t now_msec, const char *if_filter)
{
    if (!inst || !inst->neighbors)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        const isis_neighbor_t *nbr = (const isis_neighbor_t *)value;
        if (isis_neighbor_should_remove(inst, nbr, now_msec, if_filter))
        {
            isis_neighbor_withdraw_learned_all(inst, nbr);
            isis_spf_withdraw_neighbor_routes(inst, nbr);
            isis_lsp_remove_origin(inst, nbr->level, nbr->system_id);
            g_hash_table_iter_remove(&iter);
            continue;
        }
        isis_neighbor_reconcile_learned(inst, nbr);
    }

    isis_lsp_reconcile_instance(inst, now_msec);
    isis_spf_reconcile_instance(inst);
}

void isis_neighbor_on_if_removed(isis_instance_cfg_t *inst, const char *ifname)
{
    if (!inst || !inst->neighbors || !ifname || ifname[0] == '\0')
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        const isis_neighbor_t *nbr = (const isis_neighbor_t *)value;
        if (nbr && strcmp(nbr->ifname, ifname) == 0)
        {
            isis_neighbor_withdraw_learned_all(inst, nbr);
            isis_spf_withdraw_neighbor_routes(inst, nbr);
            isis_lsp_remove_origin(inst, nbr->level, nbr->system_id);
            g_hash_table_iter_remove(&iter);
        }
    }
}

void isis_neighbor_reconcile_instance(isis_instance_cfg_t *inst)
{
    isis_neighbor_reconcile_instance_now(inst, isis_now_msec(), NULL);
}

void isis_neighbor_reconcile_if(const char *ifname)
{
    if (!g_isis_work_local || !g_isis_work_local->instances)
    {
        return;
    }

    uint64_t now_msec = isis_now_msec();
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_isis_work_local->instances);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        isis_neighbor_reconcile_instance_now((isis_instance_cfg_t *)value, now_msec, ifname);
    }
}

void isis_neighbor_reconcile_all(void)
{
    if (!g_isis_work_local || !g_isis_work_local->instances)
    {
        return;
    }

    uint64_t now_msec = isis_now_msec();
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_isis_work_local->instances);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        isis_neighbor_reconcile_instance_now((isis_instance_cfg_t *)value, now_msec, NULL);
    }
}

typedef struct isis_rx_neighbor_ctx
{
    const char *ifname;
    uint8_t level;
    uint8_t system_id[6];
    uint16_t hold_time_sec;
    uint8_t priority;
    uint8_t has_ipv4;
    uint8_t has_ipv6;
    net_addr_t ipv4_addr;
    net_addr_t ipv6_addr;
    uint64_t now_msec;
} isis_rx_neighbor_ctx_t;

static void isis_rx_neighbor_apply_instance(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    isis_instance_cfg_t *inst = (isis_instance_cfg_t *)value;
    isis_rx_neighbor_ctx_t *ctx = (isis_rx_neighbor_ctx_t *)user_data;
    if (!inst || !ctx || !ctx->ifname || ctx->ifname[0] == '\0' || !inst->neighbors || !inst->if_cfgs)
    {
        return;
    }

    if (!inst->admin_up || !isis_level_enabled(inst, ctx->level))
    {
        return;
    }

    const isis_if_cfg_t *if_cfg = (const isis_if_cfg_t *)g_hash_table_lookup(inst->if_cfgs, ctx->ifname);
    if (!if_cfg || !isis_neighbor_pick_tx_af_cfg(inst, if_cfg))
    {
        return;
    }

    uint8_t local_sysid[6];
    if (isis_extract_system_id(inst->net, local_sysid) == 0 && memcmp(local_sysid, ctx->system_id, 6u) == 0)
    {
        return;
    }

    char key_buf[ISIS_NEIGHBOR_KEY_MAX];
    isis_neighbor_key_format(key_buf, sizeof(key_buf), ctx->ifname, ctx->level, ctx->system_id);

    isis_neighbor_t *nbr = (isis_neighbor_t *)g_hash_table_lookup(inst->neighbors, key_buf);
    if (!nbr)
    {
        nbr = g_malloc0(sizeof(*nbr));
        if (!nbr)
        {
            return;
        }
        nbr->state = ISIS_ADJ_STATE_INIT;
        g_strlcpy(nbr->ifname, ctx->ifname, sizeof(nbr->ifname));
        memcpy(nbr->system_id, ctx->system_id, sizeof(nbr->system_id));
        nbr->level = ctx->level;
        nbr->priority = ctx->priority;
        nbr->hold_time_sec = (ctx->hold_time_sec == 0u) ? ISIS_DEFAULT_HOLD_TIME_SEC : ctx->hold_time_sec;
        nbr->ipv4_addr = ctx->has_ipv4 ? ctx->ipv4_addr : (net_addr_t){0};
        nbr->ipv6_addr = ctx->has_ipv6 ? ctx->ipv6_addr : (net_addr_t){0};
        nbr->last_seen_msec = ctx->now_msec;
        g_hash_table_replace(inst->neighbors, g_strdup(key_buf), nbr);
        isis_neighbor_reconcile_learned(inst, nbr);
        return;
    }

    nbr->state = ISIS_ADJ_STATE_UP;
    nbr->priority = ctx->priority;
    nbr->hold_time_sec = (ctx->hold_time_sec == 0u) ? ISIS_DEFAULT_HOLD_TIME_SEC : ctx->hold_time_sec;
    nbr->ipv4_addr = ctx->has_ipv4 ? ctx->ipv4_addr : (net_addr_t){0};
    nbr->ipv6_addr = ctx->has_ipv6 ? ctx->ipv6_addr : (net_addr_t){0};
    nbr->last_seen_msec = ctx->now_msec;
    isis_neighbor_reconcile_learned(inst, nbr);
}

static void isis_handle_iih_payload(const uint8_t *pdu, size_t pdu_len, const struct sockaddr_ll *sll)
{
    if (!pdu || pdu_len < ISIS_LAN_IIH_HDR_LEN || !sll)
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
    if (pdu_type == ISIS_PDU_TYPE_LAN_IIH_L1)
    {
        level = 1u;
    }
    else if (pdu_type == ISIS_PDU_TYPE_LAN_IIH_L2)
    {
        level = 2u;
    }
    else
    {
        return;
    }

    if ((pdu[3] != 0u && pdu[3] != 6u) || pdu[1] < ISIS_LAN_IIH_HDR_LEN)
    {
        return;
    }

    uint16_t pdu_declared_len = (uint16_t)(((uint16_t)pdu[17] << 8) | pdu[18]);
    if (pdu_declared_len >= ISIS_LAN_IIH_HDR_LEN && pdu_declared_len <= pdu_len)
    {
        pdu_len = pdu_declared_len;
    }
    if (pdu_len < ISIS_LAN_IIH_HDR_LEN)
    {
        return;
    }

    uint32_t ifindex = (sll->sll_ifindex > 0) ? (uint32_t)sll->sll_ifindex : 0u;
    const char *ifname = if_api_cache_get_logical_name(ifindex);
    if (!ifname || ifname[0] == '\0')
    {
        return;
    }

    isis_rx_neighbor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ifname = ifname;
    ctx.level = level;
    memcpy(ctx.system_id, &pdu[9], sizeof(ctx.system_id));
    ctx.hold_time_sec = (uint16_t)(((uint16_t)pdu[15] << 8) | pdu[16]);
    ctx.priority = pdu[19];
    ctx.now_msec = isis_now_msec();

    size_t pos = ISIS_LAN_IIH_HDR_LEN;
    while (pos + 2u <= pdu_len)
    {
        uint8_t tlv_type = pdu[pos];
        uint8_t tlv_len = pdu[pos + 1u];
        pos += 2u;
        if (pos + tlv_len > pdu_len)
        {
            break;
        }

        const uint8_t *tlv_val = &pdu[pos];
        if (tlv_type == ISIS_TLV_IPV4_INTF_ADDR && tlv_len >= 4u && !ctx.has_ipv4)
        {
            memset(&ctx.ipv4_addr, 0, sizeof(ctx.ipv4_addr));
            ctx.ipv4_addr.family = AF_INET;
            memcpy(&ctx.ipv4_addr.u.v4, tlv_val, 4u);
            ctx.has_ipv4 = 1u;
        }
        else if (tlv_type == ISIS_TLV_IPV6_INTF_ADDR && tlv_len >= 16u && !ctx.has_ipv6)
        {
            memset(&ctx.ipv6_addr, 0, sizeof(ctx.ipv6_addr));
            ctx.ipv6_addr.family = AF_INET6;
            memcpy(&ctx.ipv6_addr.u.v6, tlv_val, 16u);
            ctx.has_ipv6 = 1u;
        }

        pos += tlv_len;
    }

    g_hash_table_foreach(g_isis_work_local->instances, isis_rx_neighbor_apply_instance, &ctx);
}

void isis_neighbor_handle_raw_event(void)
{
    if (g_isis_neighbor_local.raw_fd < 0)
    {
        return;
    }

    for (;;)
    {
        uint8_t buf[ISIS_NEIGHBOR_PKT_MAX];
        struct sockaddr_ll sll;
        socklen_t sll_len = sizeof(sll);
        ssize_t n = recvfrom(g_isis_neighbor_local.raw_fd, buf, sizeof(buf), 0, (struct sockaddr *)&sll, &sll_len);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            LOG_PERROR("ISIS: raw recvfrom failed");
            break;
        }
        if ((size_t)n < (ETH_HLEN + 3u + ISIS_LAN_IIH_HDR_LEN))
        {
            continue;
        }
        if (sll.sll_pkttype == PACKET_OUTGOING)
        {
            continue;
        }

        const uint8_t *frame = buf;
        uint16_t len8023 = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
        if (len8023 >= 0x0600u)
        {
            continue;
        }
        size_t payload_avail = (size_t)n - ETH_HLEN;
        if (len8023 > payload_avail)
        {
            len8023 = (uint16_t)payload_avail;
        }
        if (len8023 < (3u + ISIS_LAN_IIH_HDR_LEN))
        {
            continue;
        }

        const uint8_t *llc = frame + ETH_HLEN;
        if (llc[0] != ISIS_LLC_DSAP || llc[1] != ISIS_LLC_SSAP || llc[2] != ISIS_LLC_CTRL)
        {
            continue;
        }

        const uint8_t *pdu = llc + 3u;
        size_t pdu_len = len8023 - 3u;
        isis_handle_iih_payload(pdu, pdu_len, &sll);
        isis_lsp_handle_pdu(g_isis_neighbor_local.raw_fd, pdu, pdu_len, &sll);
    }
}

void isis_neighbor_handle_tick_event(void)
{
    if (g_isis_neighbor_local.tick_fd < 0)
    {
        return;
    }

    uint64_t expirations = 0;
    while (read(g_isis_neighbor_local.tick_fd, &expirations, sizeof(expirations)) > 0)
    {
        /* drain */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("ISIS: tick timerfd read failed");
    }

    uint64_t now_msec = isis_now_msec();
    if (g_isis_work_local && g_isis_work_local->instances)
    {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, g_isis_work_local->instances);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            (void)key;
            isis_instance_cfg_t *inst = (isis_instance_cfg_t *)value;
            isis_send_hello_instance(inst, now_msec);
            isis_lsp_send_due(inst, g_isis_neighbor_local.raw_fd, now_msec);
        }
    }

    isis_neighbor_reconcile_all();
}

static void isis_neighbor_close_fd(int epoll_fd, int *fd_io)
{
    if (!fd_io || *fd_io < 0)
    {
        return;
    }
    if (epoll_fd >= 0)
    {
        (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, *fd_io, NULL);
    }
    close(*fd_io);
    *fd_io = -1;
}

int isis_neighbor_prepare(int epoll_fd, void *raw_tag, void *tick_tag)
{
    g_isis_neighbor_local.raw_tag = raw_tag;
    g_isis_neighbor_local.tick_tag = tick_tag;

    g_isis_neighbor_local.raw_fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (g_isis_neighbor_local.raw_fd < 0)
    {
        LOG_WARN("ISIS: raw socket disabled (%s)", strerror(errno));
    }
    else if (epoll_fd >= 0)
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = g_isis_neighbor_local.raw_tag;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_isis_neighbor_local.raw_fd, &ev) < 0)
        {
            LOG_WARN("ISIS: epoll add raw_fd failed (%s)", strerror(errno));
            isis_neighbor_close_fd(epoll_fd, &g_isis_neighbor_local.raw_fd);
        }
    }

    g_isis_neighbor_local.tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_isis_neighbor_local.tick_fd < 0)
    {
        LOG_WARN("ISIS: timerfd disabled (%s)", strerror(errno));
        return ERRCODE_SUCCESS;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = ISIS_HELLO_TICK_SEC;
    its.it_interval.tv_sec = ISIS_HELLO_TICK_SEC;
    if (timerfd_settime(g_isis_neighbor_local.tick_fd, 0, &its, NULL) < 0)
    {
        LOG_WARN("ISIS: timerfd_settime failed (%s)", strerror(errno));
        isis_neighbor_close_fd(epoll_fd, &g_isis_neighbor_local.tick_fd);
        return ERRCODE_SUCCESS;
    }

    if (epoll_fd >= 0)
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = g_isis_neighbor_local.tick_tag;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_isis_neighbor_local.tick_fd, &ev) < 0)
        {
            LOG_WARN("ISIS: epoll add tick_fd failed (%s)", strerror(errno));
            isis_neighbor_close_fd(epoll_fd, &g_isis_neighbor_local.tick_fd);
            return ERRCODE_SUCCESS;
        }
    }

    return ERRCODE_SUCCESS;
}

void isis_neighbor_shutdown(int epoll_fd)
{
    isis_neighbor_close_fd(epoll_fd, &g_isis_neighbor_local.raw_fd);
    isis_neighbor_close_fd(epoll_fd, &g_isis_neighbor_local.tick_fd);
    g_isis_neighbor_local.raw_tag = NULL;
    g_isis_neighbor_local.tick_tag = NULL;
}
