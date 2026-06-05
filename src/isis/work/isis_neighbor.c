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
#include "if.h"
#include "isis.h"
#include "isis_lsp.h"
#include "isis_route.h"
#include "isis_route_sync.h"
#include "isis_spf.h"
#include "log.h"
#include "route.h"

#define ISIS_NEIGHBOR_PKT_MAX 2048u
#define ISIS_NEIGHBOR_KEY_MAX (IF_LOGICAL_NAME_MAX + 32u)
#define ISIS_LEARNED_ROUTE_KEY_MAX (ISIS_NEIGHBOR_KEY_MAX + 24u)
#define ISIS_ROUTE_PATH_KEY_MAX (ISIS_LEARNED_ROUTE_KEY_MAX + 160u)
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
#define ISIS_IS_NEIGHBORS_TLV_MAX 252u

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

/* 前向声明（实现在本文件后半部分） */
static int isis_extract_system_id(const char *net, uint8_t sysid[6]);
static int isis_get_src_mac(int raw_fd, const if_api_cache_entry_t *if_entry, uint8_t out[ETH_ALEN]);

/** 为 if_cfg 的 DIS 状态分配一个稳定的非零 circuit-id（按 ifindex 派生） */
static uint8_t isis_dis_circuit_id_for(const if_api_cache_entry_t *if_entry)
{
    if (!if_entry || if_entry->ifindex == 0u)
    {
        return 1u;
    }
    uint8_t cid = (uint8_t)((if_entry->ifindex & 0xFFu));
    if (cid == 0u)
    {
        cid = 1u;
    }
    return cid;
}

/** 比较 (priority, snpa)：返回 >0 表示 a 胜出，<0 表示 b 胜出，0 相等 */
static int isis_dis_compare(uint8_t pri_a, const uint8_t snpa_a[ETH_ALEN], uint8_t pri_b,
                            const uint8_t snpa_b[ETH_ALEN])
{
    if (pri_a != pri_b)
    {
        return (pri_a > pri_b) ? 1 : -1;
    }
    return memcmp(snpa_a, snpa_b, ETH_ALEN);
}

/** 在 (instance, ifname, level) 上跑一次 DIS 选举：从 ourselves + 所有 UP 邻居中
 *  选出 (priority, SNPA) 最大者；更新 if_cfg->dis_l1/l2 状态 */
static void isis_dis_run_election(isis_instance_cfg_t *inst, const char *ifname, uint8_t level, uint64_t now_msec)
{
    if (!inst || !inst->if_cfgs || !inst->neighbors || !ifname)
    {
        return;
    }

    isis_if_cfg_t *if_cfg = (isis_if_cfg_t *)g_hash_table_lookup(inst->if_cfgs, ifname);
    if (!if_cfg)
    {
        return;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(ifname);
    if (!if_entry || if_entry->ifindex == 0u)
    {
        return;
    }

    uint8_t local_sysid[6] = {0};
    if (isis_extract_system_id(inst->net, local_sysid) != 0)
    {
        return;
    }

    /* 候选先放本机 */
    uint8_t winner_pri = 64u; /* TODO: 可配置；目前固定 64 */
    uint8_t winner_snpa[ETH_ALEN] = {0};
    if (isis_get_src_mac(g_isis_neighbor_local.raw_fd, if_entry, winner_snpa) != 0)
    {
        return;
    }
    uint8_t winner_sysid[6];
    memcpy(winner_sysid, local_sysid, 6u);
    uint8_t winner_circuit_id = isis_dis_circuit_id_for(if_entry);
    uint8_t winner_is_local = 1u;
    uint8_t winner_remote_lan_id[7] = {0};

    /* 扫描该 (ifname, level) 上所有 UP 邻居 */
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
        if (isis_dis_compare(nbr->priority, nbr->remote_snpa, winner_pri, winner_snpa) > 0)
        {
            winner_pri = nbr->priority;
            memcpy(winner_snpa, nbr->remote_snpa, ETH_ALEN);
            memcpy(winner_sysid, nbr->system_id, 6u);
            memcpy(winner_remote_lan_id, nbr->remote_lan_id, 7u);
            winner_is_local = 0u;
        }
    }

    isis_dis_state_t *dis = (level == 1u) ? &if_cfg->dis_l1 : &if_cfg->dis_l2;
    uint8_t prev_we_dis = dis->we_are_dis;
    uint8_t prev_lan_id[7];
    memcpy(prev_lan_id, dis->lan_id, 7u);

    if (winner_is_local)
    {
        dis->we_are_dis = 1u;
        dis->our_circuit_id = winner_circuit_id;
        memcpy(dis->lan_id, local_sysid, 6u);
        dis->lan_id[6] = winner_circuit_id;
    }
    else
    {
        dis->we_are_dis = 0u;
        /* 邻居告诉我们他认为 DIS 是谁（remote_lan_id），优先采纳；否则用 winner_sysid+1 兜底 */
        if (winner_remote_lan_id[6] != 0u)
        {
            memcpy(dis->lan_id, winner_remote_lan_id, 7u);
        }
        else
        {
            memcpy(dis->lan_id, winner_sysid, 6u);
            dis->lan_id[6] = 1u;
        }
    }
    dis->last_election_msec = now_msec;

    if (dis->we_are_dis && !prev_we_dis)
    {
        /* 刚成为 DIS：bump 伪节点 LSP seq，等下一个 LSP tick 发出 */
        dis->pseudo_seq += 1u;
    }
    /* LAN-ID 变更也会让 IS reach TLV 内容变（指向新的 pseudonode），LSP 自然在下个 tick 重发新版本 */
    (void)prev_lan_id;
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

static int isis_mac_is_zero(const uint8_t mac[ETH_ALEN])
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

static int isis_mac_equal(const uint8_t a[ETH_ALEN], const uint8_t b[ETH_ALEN])
{
    if (!a || !b)
    {
        return 0;
    }
    return memcmp(a, b, ETH_ALEN) == 0 ? 1 : 0;
}

static uint8_t isis_instance_circuit_type(const isis_instance_cfg_t *inst)
{
    if (!inst)
    {
        return 0u;
    }
    if (inst->is_type == ISIS_IS_TYPE_LEVEL_1)
    {
        return 1u;
    }
    if (inst->is_type == ISIS_IS_TYPE_LEVEL_2)
    {
        return 2u;
    }
    return 3u;
}

static int isis_circuit_type_supports_level(uint8_t circuit_type, uint8_t level)
{
    if (level == 1u)
    {
        return (circuit_type & 0x1u) ? 1 : 0;
    }
    if (level == 2u)
    {
        return (circuit_type & 0x2u) ? 1 : 0;
    }
    return 0;
}

static int isis_area_list_contains(const uint8_t *val, size_t val_len, const uint8_t *area, size_t area_len)
{
    if (!val || val_len == 0u || !area || area_len == 0u)
    {
        return 0;
    }

    size_t pos = 0u;
    while (pos < val_len)
    {
        uint8_t len = val[pos++];
        if (len == 0u || pos + len > val_len)
        {
            break;
        }
        if ((size_t)len == area_len && memcmp(&val[pos], area, area_len) == 0)
        {
            return 1;
        }
        pos += len;
    }
    return 0;
}

static int isis_snpa_list_contains(const uint8_t *val, size_t val_len, const uint8_t snpa[ETH_ALEN])
{
    if (!val || val_len < ETH_ALEN || !snpa || isis_mac_is_zero(snpa))
    {
        return 0;
    }

    for (size_t pos = 0u; pos + ETH_ALEN <= val_len; pos += ETH_ALEN)
    {
        if (isis_mac_equal(&val[pos], snpa))
        {
            return 1;
        }
    }
    return 0;
}

static size_t isis_collect_is_neighbors_tlv(const isis_instance_cfg_t *inst, const char *ifname, uint8_t level,
                                            uint8_t *out, size_t out_cap)
{
    if (!inst || !inst->neighbors || !ifname || ifname[0] == '\0' || !out || out_cap < ETH_ALEN)
    {
        return 0u;
    }

    size_t len = 0u;
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        const isis_neighbor_t *nbr = (const isis_neighbor_t *)value;
        if (!nbr || nbr->level != level || strcmp(nbr->ifname, ifname) != 0 || !nbr->hello_valid ||
            isis_mac_is_zero(nbr->remote_snpa))
        {
            continue;
        }

        if (len + ETH_ALEN > out_cap)
        {
            break;
        }

        int duplicate = 0;
        for (size_t pos = 0u; pos + ETH_ALEN <= len; pos += ETH_ALEN)
        {
            if (isis_mac_equal(&out[pos], nbr->remote_snpa))
            {
                duplicate = 1;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }

        memcpy(&out[len], nbr->remote_snpa, ETH_ALEN);
        len += ETH_ALEN;
    }

    return len;
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

static int isis_build_iih_pdu(const isis_instance_cfg_t *inst, const isis_if_cfg_t *if_cfg,
                              const if_api_cache_entry_t *if_entry, uint8_t level, const uint8_t system_id[6],
                              uint16_t hold_time_sec, uint8_t *pdu, size_t pdu_cap, size_t *pdu_len_out)
{
    if (!inst || !if_cfg || !if_entry || !system_id || !pdu || !pdu_len_out || pdu_cap < ISIS_LAN_IIH_HDR_LEN)
    {
        return -1;
    }

    const isis_if_af_cfg_t *af_cfg_v4 = isis_neighbor_if_af_cfg(inst, if_cfg, ROUTE_AFI_IPV4);
    const isis_if_af_cfg_t *af_cfg_v6 = isis_neighbor_if_af_cfg(inst, if_cfg, ROUTE_AFI_IPV6);
    int adv_v4 = (af_cfg_v4 && !af_cfg_v4->passive) ? 1 : 0;
    int adv_v6 = (af_cfg_v6 && !af_cfg_v6->passive) ? 1 : 0;

    size_t p = 0;
    pdu[p++] = ISIS_NLPID;
    pdu[p++] = ISIS_LAN_IIH_HDR_LEN;
    pdu[p++] = 1u;
    pdu[p++] = 6u;
    pdu[p++] = (level == 1u) ? ISIS_PDU_TYPE_LAN_IIH_L1 : ISIS_PDU_TYPE_LAN_IIH_L2;
    pdu[p++] = 1u;
    pdu[p++] = 0u;
    pdu[p++] = 3u;

    uint8_t circuit_type = isis_instance_circuit_type(inst);
    pdu[p++] = circuit_type;

    memcpy(&pdu[p], system_id, 6u);
    p += 6u;

    pdu[p++] = (uint8_t)((hold_time_sec >> 8) & 0xFFu);
    pdu[p++] = (uint8_t)(hold_time_sec & 0xFFu);

    size_t pdu_len_pos = p;
    pdu[p++] = 0u;
    pdu[p++] = 0u;

    pdu[p++] = 64u; /* priority */

    /* LAN-ID：写本机当前所认的 DIS LAN-ID（sysid 6B + circuit-id 1B）。
     * 若刚启动还没选举（lan_id 全 0），临时用 own_sysid + own_circuit_id 占位，
     * 让对端能尝试匹配；选举跑过后会被正确覆盖。 */
    const isis_dis_state_t *dis = (level == 1u) ? &if_cfg->dis_l1 : &if_cfg->dis_l2;
    uint8_t lan_id_out[7];
    if (dis->lan_id[6] != 0u || dis->we_are_dis)
    {
        memcpy(lan_id_out, dis->lan_id, 7u);
        if (lan_id_out[6] == 0u && dis->we_are_dis)
        {
            lan_id_out[6] = dis->our_circuit_id ? dis->our_circuit_id : isis_dis_circuit_id_for(if_entry);
        }
    }
    else
    {
        memcpy(lan_id_out, system_id, 6u);
        lan_id_out[6] = isis_dis_circuit_id_for(if_entry);
    }
    memcpy(&pdu[p], lan_id_out, 7u);
    p += 7u;

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
    if (adv_v4)
    {
        nlpids[nlpids_len++] = ISIS_NLPID_IPV4;
    }
    if (adv_v6)
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

    if (adv_v4 && if_entry->ipv4_addr.family == AF_INET)
    {
        if (isis_tlv_append(pdu, pdu_cap, &p, ISIS_TLV_IPV4_INTF_ADDR, &if_entry->ipv4_addr.u.v4, 4u) != 0)
        {
            return -1;
        }
    }
    if (adv_v6 && if_entry->ipv6_linklocal_addr.family == AF_INET6)
    {
        if (isis_tlv_append(pdu, pdu_cap, &p, ISIS_TLV_IPV6_INTF_ADDR, &if_entry->ipv6_linklocal_addr.u.v6, 16u) != 0)
        {
            return -1;
        }
    }

    uint8_t neighbors_tlv[ISIS_IS_NEIGHBORS_TLV_MAX];
    size_t neighbors_tlv_len =
        isis_collect_is_neighbors_tlv(inst, if_entry->logical_name, level, neighbors_tlv, sizeof(neighbors_tlv));
    if (isis_tlv_append(pdu, pdu_cap, &p, ISIS_TLV_IS_NEIGHBORS, (neighbors_tlv_len > 0u) ? neighbors_tlv : NULL,
                        (uint8_t)neighbors_tlv_len) != 0)
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
    if (isis_build_iih_pdu(inst, if_cfg, if_entry, level, system_id, hold_time_sec, pdu, sizeof(pdu), &pdu_len) != 0)
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
    if (!if_entry || !if_entry->proto_up || if_entry->ifindex == 0u)
    {
        return;
    }

    /* hello tick 也跑一次选举，保证 dis 状态在没收到对端 IIH 时也能初始化（自己单独时当 DIS） */
    if (isis_level_enabled(ctx->inst, 1u))
    {
        isis_dis_run_election(ctx->inst, if_cfg->ifname, 1u, ctx->now_msec);
        isis_send_iih_on_if(ctx->inst, if_cfg, if_entry, 1u);
    }
    if (isis_level_enabled(ctx->inst, 2u))
    {
        isis_dis_run_election(ctx->inst, if_cfg->ifname, 2u, ctx->now_msec);
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
    if (!inst || !inst->learned_route_heads || !nbr)
    {
        return;
    }

    char key_buf[ISIS_LEARNED_ROUTE_KEY_MAX];
    isis_learned_route_key_format(key_buf, sizeof(key_buf), nbr->ifname, nbr->level, nbr->system_id, afi);

    const isis_route_head_t *head = (const isis_route_head_t *)g_hash_table_lookup(inst->learned_route_heads, key_buf);
    const isis_route_path_t *best = isis_route_head_best_path(head);
    if (best)
    {
        (void)isis_route_sync_publish_del(&best->state);
    }
    (void)g_hash_table_remove(inst->learned_route_heads, key_buf);
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
    if (!inst || !nbr || !inst->learned_route_heads)
    {
        return;
    }

    char key_buf[ISIS_LEARNED_ROUTE_KEY_MAX];
    isis_learned_route_key_format(key_buf, sizeof(key_buf), nbr->ifname, nbr->level, nbr->system_id, afi);
    const isis_route_head_t *current_head =
        (const isis_route_head_t *)g_hash_table_lookup(inst->learned_route_heads, key_buf);
    const isis_route_path_t *current_best = isis_route_head_best_path(current_head);
    const isis_route_state_t *current = current_best ? &current_best->state : NULL;

    int af_enabled = (afi == ROUTE_AFI_IPV4) ? (inst->af_ipv4 != 0u) : (inst->af_ipv6 != 0u);
    const isis_if_af_cfg_t *af_cfg = isis_neighbor_if_af_cfg(inst, if_cfg, afi);
    int has_desired = 0;
    isis_route_state_t desired;
    memset(&desired, 0, sizeof(desired));

    if (nbr->state == ISIS_ADJ_STATE_UP && inst->admin_up && af_enabled && af_cfg && if_entry && if_entry->proto_up &&
        if_entry->ifindex != 0u && !af_cfg->passive && isis_level_enabled(inst, nbr->level))
    {
        isis_nexthop_table_t *nh_table = isis_instance_nexthop_table(inst, afi);
        if (!nh_table)
        {
            return;
        }
        desired.afi = afi;
        desired.metric = ((af_cfg->metric == 0u) ? ISIS_DEFAULT_IF_METRIC : af_cfg->metric) + ISIS_NEIGHBOR_ROUTE_COST;

        if (afi == ROUTE_AFI_IPV4 && nbr->ipv4_addr.family == AF_INET)
        {
            desired.prefix_len = 32u;
            desired.prefix_addr = nbr->ipv4_addr;
            net_addr_t zero_nh;
            isis_zero_addr(AF_INET, &zero_nh);
            if (isis_route_state_set_nexthop(&desired, nh_table, if_entry->ifindex, if_entry->ifindex, &zero_nh) !=
                ERRCODE_SUCCESS)
            {
                return;
            }
            if (if_entry->ipv4_addr.family == AF_INET)
            {
                desired.source_addr = if_entry->ipv4_addr;
            }
            else
            {
                isis_zero_addr(AF_INET, &desired.source_addr);
            }

            /* 邻居与本端 iface 同 IPv4 子网（典型 transit link） → connected /30 已
             * 覆盖到对端，不必再装一条冗余的 /32。这条 /32 没带 PREFSRC 时，
             * kernel 会按 iface 上 IP 添加顺序乱选源地址（如 docker 管理网 IP），
             * 导致上层（LDP TCP 等）从错误的接口 IP 发起连接。
             * 自己 IP == 邻居 IP（退化情况）也跳过。
             * 对于 unnumbered / loopback 邻居（IP 不在本端子网内），仍按 /32 装，
             * 用于 nexthop 解析。*/
            if (if_entry->ipv4_addr.family == AF_INET && if_entry->ipv4_prefix_len > 0u &&
                if_entry->ipv4_prefix_len <= 32u)
            {
                net_addr_t self_net = if_entry->ipv4_addr;
                net_addr_t nbr_net = nbr->ipv4_addr;
                if (net_addr_prefix_normalize(&self_net, if_entry->ipv4_prefix_len) == 0 &&
                    net_addr_prefix_normalize(&nbr_net, if_entry->ipv4_prefix_len) == 0 &&
                    !net_addr_equal(&self_net, &nbr_net))
                {
                    has_desired = 1;
                }
            }
            else
            {
                has_desired = 1;
            }
        }
        else if (afi == ROUTE_AFI_IPV6 && nbr->ipv6_addr.family == AF_INET6)
        {
            desired.prefix_len = 128u;
            desired.prefix_addr = nbr->ipv6_addr;
            net_addr_t zero_nh;
            isis_zero_addr(AF_INET6, &zero_nh);
            if (isis_route_state_set_nexthop(&desired, nh_table, if_entry->ifindex, if_entry->ifindex, &zero_nh) !=
                ERRCODE_SUCCESS)
            {
                return;
            }
            if (if_entry->ipv6_addr.family == AF_INET6)
            {
                desired.source_addr = if_entry->ipv6_addr;
            }
            else
            {
                isis_zero_addr(AF_INET6, &desired.source_addr);
            }

            /* 同 IPv4 注释，IPv6 同样处理 */
            if (if_entry->ipv6_addr.family == AF_INET6 && if_entry->ipv6_prefix_len > 0u &&
                if_entry->ipv6_prefix_len <= 128u)
            {
                net_addr_t self_net = if_entry->ipv6_addr;
                net_addr_t nbr_net = nbr->ipv6_addr;
                if (net_addr_prefix_normalize(&self_net, if_entry->ipv6_prefix_len) == 0 &&
                    net_addr_prefix_normalize(&nbr_net, if_entry->ipv6_prefix_len) == 0 &&
                    !net_addr_equal(&self_net, &nbr_net))
                {
                    has_desired = 1;
                }
            }
            else
            {
                has_desired = 1;
            }
        }
    }

    if (!has_desired)
    {
        isis_route_state_reset(&desired);
        if (current)
        {
            (void)isis_route_sync_publish_del(current);
            (void)g_hash_table_remove(inst->learned_route_heads, key_buf);
        }
        return;
    }

    if (current && isis_route_state_same(current, &desired))
    {
        isis_route_state_reset(&desired);
        return;
    }

    if (current)
    {
        (void)isis_route_sync_publish_del(current);
        (void)g_hash_table_remove(inst->learned_route_heads, key_buf);
    }

    if (isis_route_sync_publish_add(&desired) != ERRCODE_SUCCESS)
    {
        isis_route_state_reset(&desired);
        return;
    }

    char path_key[ISIS_ROUTE_PATH_KEY_MAX];
    isis_route_path_key_format(path_key, sizeof(path_key), key_buf, &desired);
    isis_route_head_table_add_path(inst->learned_route_heads, key_buf, path_key, &desired);
    isis_route_state_reset(&desired);
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
    if (!if_entry || !if_entry->proto_up || if_entry->ifindex == 0u)
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
    const if_api_cache_entry_t *if_entry;
    uint8_t level;
    uint8_t system_id[6];
    uint16_t hold_time_sec;
    uint8_t priority;
    uint8_t remote_lan_id[7]; /**< 邻居在 IIH 里写的 LAN-ID（DIS sysid+circuit-id） */
    uint8_t local_snpa[ETH_ALEN];
    uint8_t remote_snpa[ETH_ALEN];
    uint8_t remote_circuit_type;
    uint8_t remote_ipv4_nlpid;
    uint8_t remote_ipv6_nlpid;
    uint8_t seen_self;
    uint8_t has_ipv4;
    uint8_t has_ipv6;
    uint8_t area_tlv[255];
    uint16_t area_tlv_len;
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

    const isis_if_af_cfg_t *cfg_v4 = isis_neighbor_if_af_cfg(inst, if_cfg, ROUTE_AFI_IPV4);
    const isis_if_af_cfg_t *cfg_v6 = isis_neighbor_if_af_cfg(inst, if_cfg, ROUTE_AFI_IPV6);
    int active_v4 = (cfg_v4 && !cfg_v4->passive) ? 1 : 0;
    int active_v6 = (cfg_v6 && !cfg_v6->passive) ? 1 : 0;

    int hold_ok = (ctx->hold_time_sec != 0u) ? 1 : 0;
    int circuit_ok = isis_circuit_type_supports_level(ctx->remote_circuit_type, ctx->level);
    int nlpids_ok = ((active_v4 && ctx->remote_ipv4_nlpid) || (active_v6 && ctx->remote_ipv6_nlpid)) ? 1 : 0;
    int area_match = 1;
    if (ctx->level == 1u)
    {
        uint8_t local_area[64];
        uint8_t local_area_len = 0u;
        if (isis_extract_area(inst->net, local_area, sizeof(local_area), &local_area_len) != 0 || local_area_len == 0u)
        {
            area_match = 0;
        }
        else
        {
            area_match = isis_area_list_contains(ctx->area_tlv, ctx->area_tlv_len, local_area, local_area_len);
        }
    }

    int hello_valid = (hold_ok && circuit_ok && nlpids_ok && area_match) ? 1 : 0;
    uint8_t new_state = !hello_valid ? ISIS_ADJ_STATE_DOWN : (ctx->seen_self ? ISIS_ADJ_STATE_UP : ISIS_ADJ_STATE_INIT);

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
        g_strlcpy(nbr->ifname, ctx->ifname, sizeof(nbr->ifname));
        memcpy(nbr->system_id, ctx->system_id, sizeof(nbr->system_id));
        nbr->level = ctx->level;
        g_hash_table_replace(inst->neighbors, g_strdup(key_buf), nbr);
    }
    uint8_t prev_state = nbr->state;

    nbr->state = new_state;
    nbr->priority = ctx->priority;
    nbr->hold_time_sec = ctx->hold_time_sec;
    memcpy(nbr->local_snpa, ctx->local_snpa, sizeof(nbr->local_snpa));
    memcpy(nbr->remote_snpa, ctx->remote_snpa, sizeof(nbr->remote_snpa));
    nbr->remote_circuit_type = ctx->remote_circuit_type;
    nbr->remote_ipv4_nlpid = ctx->remote_ipv4_nlpid ? 1u : 0u;
    nbr->remote_ipv6_nlpid = ctx->remote_ipv6_nlpid ? 1u : 0u;
    nbr->seen_self = ctx->seen_self ? 1u : 0u;
    nbr->area_match = area_match ? 1u : 0u;
    nbr->circuit_ok = circuit_ok ? 1u : 0u;
    nbr->nlpids_ok = nlpids_ok ? 1u : 0u;
    nbr->hold_ok = hold_ok ? 1u : 0u;
    nbr->hello_valid = hello_valid ? 1u : 0u;
    memcpy(nbr->remote_lan_id, ctx->remote_lan_id, sizeof(nbr->remote_lan_id));
    nbr->ipv4_addr = ctx->has_ipv4 ? ctx->ipv4_addr : (net_addr_t){0};
    nbr->ipv6_addr = ctx->has_ipv6 ? ctx->ipv6_addr : (net_addr_t){0};
    nbr->last_seen_msec = ctx->now_msec;

    if (prev_state == ISIS_ADJ_STATE_UP && new_state != ISIS_ADJ_STATE_UP)
    {
        isis_spf_withdraw_neighbor_routes(inst, nbr);
        isis_lsp_remove_origin(inst, nbr->level, nbr->system_id);
    }

    /* 邻居状态变更或新邻居：跑 DIS 选举 */
    isis_dis_run_election(inst, ctx->ifname, ctx->level, ctx->now_msec);
    isis_neighbor_reconcile_learned(inst, nbr);
}

static void isis_handle_iih_payload(const uint8_t *pdu, size_t pdu_len, const struct sockaddr_ll *sll,
                                    const uint8_t src_mac[ETH_ALEN])
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
    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(ifname);
    if (!if_entry || if_entry->ifindex == 0u)
    {
        return;
    }

    isis_rx_neighbor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ifname = ifname;
    ctx.if_entry = if_entry;
    ctx.level = level;
    memcpy(ctx.system_id, &pdu[9], sizeof(ctx.system_id));
    ctx.hold_time_sec = (uint16_t)(((uint16_t)pdu[15] << 8) | pdu[16]);
    ctx.priority = pdu[19];
    memcpy(ctx.remote_lan_id, &pdu[20], 7u);
    ctx.remote_circuit_type = pdu[8];
    ctx.now_msec = isis_now_msec();
    if (src_mac && !isis_mac_is_zero(src_mac))
    {
        memcpy(ctx.remote_snpa, src_mac, sizeof(ctx.remote_snpa));
    }
    else if (sll->sll_halen >= ETH_ALEN)
    {
        memcpy(ctx.remote_snpa, sll->sll_addr, sizeof(ctx.remote_snpa));
    }
    (void)isis_get_src_mac(g_isis_neighbor_local.raw_fd, if_entry, ctx.local_snpa);

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
        if (tlv_type == ISIS_TLV_AREA_ADDR)
        {
            size_t copy_len = tlv_len;
            if (copy_len > sizeof(ctx.area_tlv) - ctx.area_tlv_len)
            {
                copy_len = sizeof(ctx.area_tlv) - ctx.area_tlv_len;
            }
            if (copy_len > 0u)
            {
                memcpy(&ctx.area_tlv[ctx.area_tlv_len], tlv_val, copy_len);
                ctx.area_tlv_len += (uint16_t)copy_len;
            }
        }
        else if (tlv_type == ISIS_TLV_PROTOCOLS_SUPPORTED)
        {
            for (uint8_t i = 0u; i < tlv_len; ++i)
            {
                if (tlv_val[i] == ISIS_NLPID_IPV4)
                {
                    ctx.remote_ipv4_nlpid = 1u;
                }
                else if (tlv_val[i] == ISIS_NLPID_IPV6)
                {
                    ctx.remote_ipv6_nlpid = 1u;
                }
            }
        }
        else if (tlv_type == ISIS_TLV_IS_NEIGHBORS)
        {
            if (!ctx.seen_self && isis_snpa_list_contains(tlv_val, tlv_len, ctx.local_snpa))
            {
                ctx.seen_self = 1u;
            }
        }
        else if (tlv_type == ISIS_TLV_IPV4_INTF_ADDR && tlv_len >= 4u && !ctx.has_ipv4)
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
        const uint8_t *src_mac = &frame[ETH_ALEN];
        isis_handle_iih_payload(pdu, pdu_len, &sll, src_mac);
        isis_lsp_handle_pdu(g_isis_neighbor_local.raw_fd, pdu, pdu_len, &sll, src_mac);
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
