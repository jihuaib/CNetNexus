/**
 * @file   isis_spf.c
 * @brief  ISIS SPF 拓扑最短路计算与前缀路由安装（IPv4/IPv6）
 * @author jhb
 * @date   2026/04/12
 */
#include "isis_spf.h"

#include <string.h>

#include "errcode.h"
#include "isis.h"
#include "isis_route.h"
#include "isis_route_sync.h"
#include "route.h"

#define ISIS_TLV_IS_REACH 2u       /* RFC 1195 narrow IS reach */
#define ISIS_TLV_IP_INT_REACH 128u /* RFC 1195 narrow IPv4 internal reach */
#define ISIS_TLV_EXT_IS_REACH 22u  /* RFC 5305 wide IS reach */
#define ISIS_TLV_EXT_IP_REACH 135u /* RFC 5305 wide IPv4 reach */
#define ISIS_TLV_IPV6_REACH 236u   /* RFC 5308 IPv6 reach */

#define ISIS_LSP_ROUTE_KEY_MAX (IF_LOGICAL_NAME_MAX + 160u)
#define ISIS_SPF_INF_DIST ((uint64_t)0x3FFFFFFFFFFFFFFFULL)
#define ISIS_SPF_NODE_ID_LEN 7u

typedef struct isis_spf_edge
{
    uint8_t to_node_id[ISIS_SPF_NODE_ID_LEN];
    uint32_t metric;
} isis_spf_edge_t;

typedef struct isis_spf_node
{
    uint8_t node_id[ISIS_SPF_NODE_ID_LEN];
    GPtrArray *edges; /* isis_spf_edge_t* */
    uint64_t dist;
    uint8_t first_hop[ISIS_SPF_NODE_ID_LEN];
    uint8_t has_first_hop;
    uint8_t visited;
} isis_spf_node_t;

typedef struct isis_spf_local_hop
{
    uint8_t node_id[ISIS_SPF_NODE_ID_LEN];
    uint32_t local_metric;
    uint32_t out_ifindex;
    net_addr_t source_addr;
    net_addr_t nexthop_addr;
} isis_spf_local_hop_t;

static void isis_spf_edge_free(gpointer data)
{
    g_free(data);
}

static void isis_spf_node_free(gpointer data)
{
    isis_spf_node_t *node = (isis_spf_node_t *)data;
    if (!node)
    {
        return;
    }
    if (node->edges)
    {
        g_ptr_array_free(node->edges, TRUE);
        node->edges = NULL;
    }
    g_free(node);
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
    if (!net || !out || out_cap == 0u || !out_len)
    {
        return -1;
    }

    size_t len = 0u;
    int high_nibble = -1;

    for (const char *p = net; *p != '\0'; ++p)
    {
        if (*p == '.' || *p == ':' || *p == '-' || g_ascii_isspace(*p))
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

    if (high_nibble >= 0 || len == 0u)
    {
        return -1;
    }

    *out_len = len;
    return 0;
}

static int isis_extract_system_id(const char *net, uint8_t sysid[6])
{
    uint8_t bytes[64];
    size_t len = 0u;
    if (!sysid || isis_parse_net_bytes(net, bytes, sizeof(bytes), &len) != 0 || len < 7u)
    {
        return -1;
    }

    memcpy(sysid, &bytes[len - 7u], 6u);
    return 0;
}

static void isis_sysid_to_hex(const uint8_t sysid[6], char *buf, size_t sz)
{
    if (!buf || sz == 0u)
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

static void isis_spf_make_node_id(const uint8_t sysid[6], uint8_t pseudonode_id, uint8_t node_id[ISIS_SPF_NODE_ID_LEN])
{
    if (!node_id)
    {
        return;
    }
    memset(node_id, 0, ISIS_SPF_NODE_ID_LEN);
    if (sysid)
    {
        memcpy(node_id, sysid, 6u);
    }
    node_id[6] = pseudonode_id;
}

static void isis_spf_node_id_to_hex(const uint8_t node_id[ISIS_SPF_NODE_ID_LEN], char *buf, size_t sz)
{
    if (!buf || sz == 0u)
    {
        return;
    }
    if (!node_id)
    {
        g_strlcpy(buf, "00000000000000", sz);
        return;
    }
    g_snprintf(buf, sz, "%02x%02x%02x%02x%02x%02x%02x", node_id[0], node_id[1], node_id[2], node_id[3], node_id[4],
               node_id[5], node_id[6]);
}

static void isis_spf_origin_prefix(char *buf, size_t sz, uint8_t level, const uint8_t origin_sysid[6])
{
    if (!buf || sz == 0u)
    {
        return;
    }
    char sysid_hex[13] = {0};
    isis_sysid_to_hex(origin_sysid, sysid_hex, sizeof(sysid_hex));
    g_snprintf(buf, sz, "lsp|%u|%s|", (unsigned)level, sysid_hex);
}

static void isis_spf_route_key_format(char *buf, size_t sz, uint8_t level, const uint8_t origin_sysid[6], uint16_t afi,
                                      const net_addr_t *prefix_addr, uint8_t prefix_len)
{
    if (!buf || sz == 0u || !prefix_addr)
    {
        return;
    }

    char key_prefix[64] = {0};
    char addr_buf[64] = {0};
    isis_spf_origin_prefix(key_prefix, sizeof(key_prefix), level, origin_sysid);
    net_addr_to_str(prefix_addr, addr_buf, sizeof(addr_buf));
    g_snprintf(buf, sz, "%s%u|%s/%u", key_prefix, (unsigned)afi, addr_buf, (unsigned)prefix_len);
}

static isis_spf_node_t *isis_spf_graph_get_node(GHashTable *nodes, const uint8_t node_id[ISIS_SPF_NODE_ID_LEN],
                                                int create_if_missing)
{
    if (!nodes || !node_id)
    {
        return NULL;
    }

    char key[15] = {0};
    isis_spf_node_id_to_hex(node_id, key, sizeof(key));

    isis_spf_node_t *node = (isis_spf_node_t *)g_hash_table_lookup(nodes, key);
    if (node || !create_if_missing)
    {
        return node;
    }

    node = g_malloc0(sizeof(*node));
    if (!node)
    {
        return NULL;
    }
    memcpy(node->node_id, node_id, sizeof(node->node_id));
    node->edges = g_ptr_array_new_with_free_func(isis_spf_edge_free);
    if (!node->edges)
    {
        g_free(node);
        return NULL;
    }
    node->dist = ISIS_SPF_INF_DIST;

    g_hash_table_replace(nodes, g_strdup(key), node);
    return node;
}

static void isis_spf_graph_add_edge(GHashTable *nodes, const uint8_t from_node_id[ISIS_SPF_NODE_ID_LEN],
                                    const uint8_t to_node_id[ISIS_SPF_NODE_ID_LEN], uint32_t metric)
{
    if (!nodes || !from_node_id || !to_node_id)
    {
        return;
    }

    isis_spf_node_t *from = isis_spf_graph_get_node(nodes, from_node_id, 1);
    (void)isis_spf_graph_get_node(nodes, to_node_id, 1);
    if (!from || !from->edges)
    {
        return;
    }

    for (guint i = 0u; i < from->edges->len; ++i)
    {
        isis_spf_edge_t *e = (isis_spf_edge_t *)g_ptr_array_index(from->edges, i);
        if (!e)
        {
            continue;
        }
        if (memcmp(e->to_node_id, to_node_id, ISIS_SPF_NODE_ID_LEN) == 0)
        {
            if (metric < e->metric)
            {
                e->metric = metric;
            }
            return;
        }
    }

    isis_spf_edge_t *edge = g_malloc0(sizeof(*edge));
    if (!edge)
    {
        return;
    }
    memcpy(edge->to_node_id, to_node_id, sizeof(edge->to_node_id));
    edge->metric = metric;
    g_ptr_array_add(from->edges, edge);
}

static int isis_spf_level_enabled(const isis_instance_cfg_t *inst, uint8_t level)
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

static int isis_spf_instance_af_enabled(const isis_instance_cfg_t *inst, uint16_t afi)
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

static const isis_if_af_cfg_t *isis_spf_if_af_cfg(const isis_instance_cfg_t *inst, const isis_if_cfg_t *if_cfg,
                                                  uint16_t afi)
{
    if (!inst || !if_cfg || !isis_spf_instance_af_enabled(inst, afi))
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

static const isis_if_af_cfg_t *isis_spf_pick_active_af_cfg(const isis_instance_cfg_t *inst, const isis_if_cfg_t *if_cfg)
{
    const isis_if_af_cfg_t *af_cfg_v4 = isis_spf_if_af_cfg(inst, if_cfg, ISIS_AFI_IPV4);
    if (af_cfg_v4 && !af_cfg_v4->passive)
    {
        return af_cfg_v4;
    }

    const isis_if_af_cfg_t *af_cfg_v6 = isis_spf_if_af_cfg(inst, if_cfg, ISIS_AFI_IPV6);
    if (af_cfg_v6 && !af_cfg_v6->passive)
    {
        return af_cfg_v6;
    }

    return NULL;
}

static void isis_spf_add_root_edges(isis_instance_cfg_t *inst, uint8_t level, const uint8_t local_sysid[6],
                                    GHashTable *nodes)
{
    if (!inst || !inst->neighbors || !inst->if_cfgs || !local_sysid || !nodes)
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
        if (!nbr || nbr->level != level || nbr->state != ISIS_ADJ_STATE_UP)
        {
            continue;
        }

        const isis_if_cfg_t *if_cfg = (const isis_if_cfg_t *)g_hash_table_lookup(inst->if_cfgs, nbr->ifname);
        const isis_if_af_cfg_t *active_af_cfg = isis_spf_pick_active_af_cfg(inst, if_cfg);
        if (!active_af_cfg)
        {
            continue;
        }

        const if_api_cache_entry_t *if_entry = if_api_cache_lookup(nbr->ifname);
        if (!if_entry || !if_entry->proto_up || if_entry->ifindex == 0u)
        {
            continue;
        }

        uint32_t metric = (active_af_cfg->metric == 0u) ? ISIS_DEFAULT_IF_METRIC : active_af_cfg->metric;
        if (metric > 0x00FFFFFFu)
        {
            metric = 0x00FFFFFFu;
        }
        uint8_t local_node_id[ISIS_SPF_NODE_ID_LEN];
        uint8_t nbr_node_id[ISIS_SPF_NODE_ID_LEN];
        isis_spf_make_node_id(local_sysid, 0u, local_node_id);
        isis_spf_make_node_id(nbr->system_id, 0u, nbr_node_id);
        isis_spf_graph_add_edge(nodes, local_node_id, nbr_node_id, metric);
    }
}

/* RFC 1195 TLV 2 (narrow IS reach) 解析：
 * 体首 1 字节 virtual flag；其后 N * 11 字节 entry：
 *   default_metric(1) + delay(1) + expense(1) + error(1) + sysid(6) + pseudo(1)
 * 仅取 default metric 低 6 位作为度量值 */
static void isis_spf_parse_narrow_is_reach_tlv(GHashTable *nodes, const uint8_t origin_node_id[ISIS_SPF_NODE_ID_LEN],
                                               const uint8_t *val, size_t val_len)
{
    if (!nodes || !origin_node_id || !val || val_len < 1u)
    {
        return;
    }

    size_t pos = 1u; /* skip virtual flag */
    while (pos + 11u <= val_len)
    {
        uint8_t default_metric = val[pos];
        if ((default_metric & 0x80u) != 0u)
        {
            pos += 11u;
            continue;
        }
        uint32_t metric = (uint32_t)(default_metric & 0x3Fu);
        const uint8_t *neighbor_id = &val[pos + 4u];

        isis_spf_graph_add_edge(nodes, origin_node_id, neighbor_id, metric);
        pos += 11u;
    }
}

/* RFC 1195 TLV 128 (narrow IPv4 internal reach) 解析：12 字节/entry：
 *   default(1) + delay(1) + expense(1) + error(1) + ipv4(4) + mask(4) */
static void isis_spf_parse_narrow_ip_reach_entries(const uint8_t *val, size_t val_len, uint8_t level,
                                                   const uint8_t origin_sysid[6], uint64_t path_metric,
                                                   const isis_spf_local_hop_t *hop, GHashTable *desired)
{
    if (!val || val_len == 0u || !origin_sysid || !hop || hop->out_ifindex == 0u || !desired)
    {
        return;
    }

    size_t pos = 0u;
    while (pos + 12u <= val_len)
    {
        uint8_t default_metric = val[pos];
        if ((default_metric & 0x80u) != 0u)
        {
            pos += 12u;
            continue;
        }
        uint32_t remote_metric = (uint32_t)(default_metric & 0x3Fu);

        struct in_addr ip_be;
        memcpy(&ip_be.s_addr, &val[pos + 4u], 4u);
        uint32_t mask_he = ((uint32_t)val[pos + 8u] << 24) | ((uint32_t)val[pos + 9u] << 16) |
                           ((uint32_t)val[pos + 10u] << 8) | (uint32_t)val[pos + 11u];
        pos += 12u;

        /* 把 mask 转 prefix_len，仅接受连续 mask */
        uint8_t prefix_len = 0u;
        if (mask_he == 0u)
        {
            prefix_len = 0u;
        }
        else
        {
            uint32_t inv = ~mask_he;
            if ((inv & (inv + 1u)) != 0u)
            {
                /* 非连续 mask，丢弃 */
                continue;
            }
            while ((mask_he & 0x80000000u) != 0u)
            {
                prefix_len++;
                mask_he <<= 1;
            }
        }

        isis_route_state_t route;
        memset(&route, 0, sizeof(route));
        route.afi = ROUTE_AFI_IPV4;
        route.prefix_len = prefix_len;
        route.out_ifindex = hop->out_ifindex;
        route.prefix_addr.family = AF_INET;
        route.prefix_addr.u.v4 = ip_be;
        if (net_addr_prefix_normalize(&route.prefix_addr, route.prefix_len) != 0)
        {
            continue;
        }

        uint64_t total_metric = path_metric + (uint64_t)remote_metric;
        route.metric = (total_metric > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)total_metric;

        route.source_addr = hop->source_addr;
        if (route.source_addr.family != AF_INET)
        {
            isis_zero_addr(AF_INET, &route.source_addr);
        }
        if (hop->nexthop_addr.family != AF_INET || net_addr_is_zero(&hop->nexthop_addr))
        {
            continue;
        }
        route.nexthop_addr = hop->nexthop_addr;
        if (route.prefix_len == 32u && net_addr_equal(&route.prefix_addr, &route.nexthop_addr))
        {
            continue;
        }

        char route_key[ISIS_LSP_ROUTE_KEY_MAX] = {0};
        char path_key[ISIS_LSP_ROUTE_KEY_MAX + 160u] = {0};
        isis_spf_route_key_format(route_key, sizeof(route_key), level, origin_sysid, route.afi, &route.prefix_addr,
                                  route.prefix_len);
        isis_route_path_key_format(path_key, sizeof(path_key), route_key, &route);
        isis_route_head_table_add_path(desired, route_key, path_key, &route);
    }
}

static void isis_spf_parse_ext_is_reach_tlv(GHashTable *nodes, const uint8_t origin_node_id[ISIS_SPF_NODE_ID_LEN],
                                            const uint8_t *val, size_t val_len)
{
    if (!nodes || !origin_node_id || !val || val_len == 0u)
    {
        return;
    }

    size_t pos = 0u;
    while (pos + 11u <= val_len)
    {
        const uint8_t *neighbor_id = &val[pos];
        uint32_t metric = ((uint32_t)val[pos + 7u] << 16) | ((uint32_t)val[pos + 8u] << 8) | (uint32_t)val[pos + 9u];
        uint8_t sub_tlv_len = val[pos + 10u];
        size_t entry_len = 11u + (size_t)sub_tlv_len;
        if (pos + entry_len > val_len)
        {
            break;
        }

        isis_spf_graph_add_edge(nodes, origin_node_id, neighbor_id, metric);

        pos += entry_len;
    }
}

static void isis_spf_collect_graph_from_lsdb(isis_instance_cfg_t *inst, uint8_t level, GHashTable *nodes)
{
    if (!inst || !inst->lsdb_entries || !nodes)
    {
        return;
    }

    const uint8_t want_narrow = (inst->cost_style == ISIS_COST_STYLE_NARROW) ? 1u : 0u;

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb_entries);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        const isis_lsdb_entry_t *entry = (const isis_lsdb_entry_t *)value;
        if (!entry || entry->level != level || !entry->tlvs || entry->tlvs->len == 0u)
        {
            continue;
        }

        const uint8_t *tlvs = entry->tlvs->data;
        size_t tlv_len = entry->tlvs->len;
        size_t pos = 0u;
        while (pos + 2u <= tlv_len)
        {
            uint8_t tlv_type = tlvs[pos];
            uint8_t len = tlvs[pos + 1u];
            pos += 2u;
            if (pos + len > tlv_len)
            {
                break;
            }

            uint8_t origin_node_id[ISIS_SPF_NODE_ID_LEN];
            isis_spf_make_node_id(entry->system_id, entry->pseudonode_id, origin_node_id);

            if (want_narrow && tlv_type == ISIS_TLV_IS_REACH)
            {
                isis_spf_parse_narrow_is_reach_tlv(nodes, origin_node_id, &tlvs[pos], len);
            }
            else if (!want_narrow && tlv_type == ISIS_TLV_EXT_IS_REACH)
            {
                isis_spf_parse_ext_is_reach_tlv(nodes, origin_node_id, &tlvs[pos], len);
            }
            pos += len;
        }
    }
}

static void isis_spf_reset_nodes(GHashTable *nodes)
{
    if (!nodes)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, nodes);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        isis_spf_node_t *node = (isis_spf_node_t *)value;
        if (!node)
        {
            continue;
        }
        node->dist = ISIS_SPF_INF_DIST;
        node->visited = 0u;
        node->has_first_hop = 0u;
        memset(node->first_hop, 0, sizeof(node->first_hop));
    }
}

static isis_spf_node_t *isis_spf_pick_next_node(GHashTable *nodes)
{
    if (!nodes)
    {
        return NULL;
    }

    isis_spf_node_t *best = NULL;
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, nodes);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        isis_spf_node_t *node = (isis_spf_node_t *)value;
        if (!node || node->visited || node->dist == ISIS_SPF_INF_DIST)
        {
            continue;
        }
        if (!best || node->dist < best->dist)
        {
            best = node;
        }
    }
    return best;
}

static void isis_spf_run_dijkstra(GHashTable *nodes, const uint8_t root_node_id[ISIS_SPF_NODE_ID_LEN])
{
    if (!nodes || !root_node_id)
    {
        return;
    }

    isis_spf_reset_nodes(nodes);

    isis_spf_node_t *root = isis_spf_graph_get_node(nodes, root_node_id, 1);
    if (!root)
    {
        return;
    }
    root->dist = 0u;

    for (;;)
    {
        isis_spf_node_t *u = isis_spf_pick_next_node(nodes);
        if (!u)
        {
            break;
        }
        u->visited = 1u;

        if (!u->edges)
        {
            continue;
        }

        for (guint i = 0u; i < u->edges->len; ++i)
        {
            const isis_spf_edge_t *edge = (const isis_spf_edge_t *)g_ptr_array_index(u->edges, i);
            if (!edge)
            {
                continue;
            }

            isis_spf_node_t *v = isis_spf_graph_get_node(nodes, edge->to_node_id, 1);
            if (!v)
            {
                continue;
            }

            uint64_t alt =
                (u->dist > (ISIS_SPF_INF_DIST - edge->metric)) ? ISIS_SPF_INF_DIST : (u->dist + edge->metric);
            if (alt < v->dist)
            {
                v->dist = alt;
                if (memcmp(u->node_id, root_node_id, ISIS_SPF_NODE_ID_LEN) == 0)
                {
                    memcpy(v->first_hop, edge->to_node_id, sizeof(v->first_hop));
                    v->has_first_hop = 1u;
                }
                else if (u->has_first_hop)
                {
                    memcpy(v->first_hop, u->first_hop, sizeof(v->first_hop));
                    v->has_first_hop = 1u;
                }
            }
        }
    }
}

static void isis_spf_local_hop_free(gpointer data)
{
    g_free(data);
}

static int isis_spf_get_distance(GHashTable *nodes, const uint8_t target_node_id[ISIS_SPF_NODE_ID_LEN],
                                 uint64_t *dist_out)
{
    if (!nodes || !target_node_id || !dist_out)
    {
        return 0;
    }

    isis_spf_node_t *node = isis_spf_graph_get_node(nodes, target_node_id, 0);
    if (!node || node->dist == ISIS_SPF_INF_DIST)
    {
        return 0;
    }

    *dist_out = node->dist;
    return 1;
}

static void isis_spf_collect_local_hops(const isis_instance_cfg_t *inst, uint8_t level, uint16_t afi, GPtrArray *hops)
{
    if (!inst || !inst->neighbors || !inst->if_cfgs || !hops)
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
        if (!nbr || nbr->level != level || nbr->state != ISIS_ADJ_STATE_UP)
        {
            continue;
        }

        const isis_if_cfg_t *if_cfg = (const isis_if_cfg_t *)g_hash_table_lookup(inst->if_cfgs, nbr->ifname);
        const isis_if_af_cfg_t *af_cfg = isis_spf_if_af_cfg(inst, if_cfg, afi);
        if (!af_cfg || af_cfg->passive)
        {
            continue;
        }

        const if_api_cache_entry_t *if_entry = if_api_cache_lookup(nbr->ifname);
        if (!if_entry || !if_entry->proto_up || if_entry->ifindex == 0u)
        {
            continue;
        }

        isis_spf_local_hop_t *hop = g_malloc0(sizeof(*hop));
        if (!hop)
        {
            continue;
        }

        isis_spf_make_node_id(nbr->system_id, 0u, hop->node_id);
        hop->local_metric = (af_cfg->metric == 0u) ? ISIS_DEFAULT_IF_METRIC : af_cfg->metric;
        if (hop->local_metric > 0x00FFFFFFu)
        {
            hop->local_metric = 0x00FFFFFFu;
        }
        hop->out_ifindex = if_entry->ifindex;

        if (afi == ROUTE_AFI_IPV4)
        {
            if (nbr->ipv4_addr.family != AF_INET || net_addr_is_zero(&nbr->ipv4_addr))
            {
                g_free(hop);
                continue;
            }
            hop->nexthop_addr = nbr->ipv4_addr;
            hop->source_addr = (if_entry->ipv4_addr.family == AF_INET) ? if_entry->ipv4_addr : (net_addr_t){0};
            if (hop->source_addr.family != AF_INET)
            {
                isis_zero_addr(AF_INET, &hop->source_addr);
            }
        }
        else if (afi == ROUTE_AFI_IPV6)
        {
            if (nbr->ipv6_addr.family != AF_INET6 || net_addr_is_zero(&nbr->ipv6_addr))
            {
                g_free(hop);
                continue;
            }
            hop->nexthop_addr = nbr->ipv6_addr;
            hop->source_addr = (if_entry->ipv6_addr.family == AF_INET6) ? if_entry->ipv6_addr : (net_addr_t){0};
            if (hop->source_addr.family != AF_INET6)
            {
                isis_zero_addr(AF_INET6, &hop->source_addr);
            }
        }
        else
        {
            g_free(hop);
            continue;
        }

        g_ptr_array_add(hops, hop);
    }
}

static void isis_spf_parse_prefix_entries(const uint8_t *val, size_t val_len, uint16_t afi, uint8_t level,
                                          const uint8_t origin_sysid[6], uint64_t path_metric,
                                          const isis_spf_local_hop_t *hop, GHashTable *desired)
{
    if (!val || val_len == 0u || !origin_sysid || !hop || hop->out_ifindex == 0u || !desired)
    {
        return;
    }

    size_t pos = 0u;
    size_t min_hdr = (afi == ROUTE_AFI_IPV4) ? 5u : 6u;
    while (pos + min_hdr <= val_len)
    {
        uint32_t remote_metric = ((uint32_t)val[pos] << 24) | ((uint32_t)val[pos + 1u] << 16) |
                                 ((uint32_t)val[pos + 2u] << 8) | (uint32_t)val[pos + 3u];
        uint8_t ctrl = val[pos + 4u];
        uint8_t prefix_len = 0u;
        uint8_t has_sub = 0u;
        if (afi == ROUTE_AFI_IPV4)
        {
            /* RFC 5305: ctrl 低 6 位为 prefix_len，bit6 为 S（sub-TLV 存在） */
            prefix_len = (uint8_t)(ctrl & 0x3Fu);
            has_sub = (uint8_t)((ctrl & 0x40u) != 0u);
            pos += 5u;
        }
        else
        {
            /* RFC 5308: prefix_len 独立字节，ctrl bit5 为 S */
            prefix_len = val[pos + 5u];
            has_sub = (uint8_t)((ctrl & 0x20u) != 0u);
            pos += 6u;
        }

        uint8_t max_prefix = (afi == ROUTE_AFI_IPV4) ? 32u : 128u;
        uint8_t pfx_bytes = (uint8_t)((prefix_len + 7u) / 8u);
        if (prefix_len > max_prefix || (afi == ROUTE_AFI_IPV4 && pfx_bytes > 4u) ||
            (afi == ROUTE_AFI_IPV6 && pfx_bytes > 16u) || pos + pfx_bytes > val_len)
        {
            break;
        }

        /* 计算 sub-TLV 尾部长度（S=1 时 prefix 后跟 1 字节 sub-TLV-len + sub-TLV 数据） */
        size_t sub_extra = 0u;
        if (has_sub)
        {
            if (pos + pfx_bytes + 1u > val_len)
            {
                break;
            }
            uint8_t sub_len = val[pos + pfx_bytes];
            sub_extra = 1u + (size_t)sub_len;
            if (pos + pfx_bytes + sub_extra > val_len)
            {
                break;
            }
        }

        isis_route_state_t route;
        memset(&route, 0, sizeof(route));
        route.afi = afi;
        route.prefix_len = prefix_len;
        route.out_ifindex = hop->out_ifindex;

        uint64_t total_metric = path_metric + (uint64_t)remote_metric;
        route.metric = (total_metric > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)total_metric;

        if (afi == ROUTE_AFI_IPV4)
        {
            route.prefix_addr.family = AF_INET;
            if (pfx_bytes > 0u)
            {
                memcpy(&route.prefix_addr.u.v4.s_addr, &val[pos], pfx_bytes);
            }
            if (net_addr_prefix_normalize(&route.prefix_addr, route.prefix_len) != 0)
            {
                pos += pfx_bytes + sub_extra;
                continue;
            }

            route.source_addr = hop->source_addr;
            if (route.source_addr.family != AF_INET)
            {
                isis_zero_addr(AF_INET, &route.source_addr);
            }

            if (hop->nexthop_addr.family != AF_INET || net_addr_is_zero(&hop->nexthop_addr))
            {
                pos += pfx_bytes + sub_extra;
                continue;
            }
            route.nexthop_addr = hop->nexthop_addr;

            if (route.prefix_len == 32u && net_addr_equal(&route.prefix_addr, &route.nexthop_addr))
            {
                pos += pfx_bytes + sub_extra;
                continue;
            }
        }
        else
        {
            route.prefix_addr.family = AF_INET6;
            if (pfx_bytes > 0u)
            {
                memcpy(route.prefix_addr.u.v6.s6_addr, &val[pos], pfx_bytes);
            }
            if (net_addr_prefix_normalize(&route.prefix_addr, route.prefix_len) != 0)
            {
                pos += pfx_bytes + sub_extra;
                continue;
            }

            route.source_addr = hop->source_addr;
            if (route.source_addr.family != AF_INET6)
            {
                isis_zero_addr(AF_INET6, &route.source_addr);
            }

            if (hop->nexthop_addr.family != AF_INET6 || net_addr_is_zero(&hop->nexthop_addr))
            {
                pos += pfx_bytes + sub_extra;
                continue;
            }
            route.nexthop_addr = hop->nexthop_addr;

            if (route.prefix_len == 128u && net_addr_equal(&route.prefix_addr, &route.nexthop_addr))
            {
                pos += pfx_bytes + sub_extra;
                continue;
            }
        }

        char route_key[ISIS_LSP_ROUTE_KEY_MAX] = {0};
        char path_key[ISIS_LSP_ROUTE_KEY_MAX + 160u] = {0};
        isis_spf_route_key_format(route_key, sizeof(route_key), level, origin_sysid, route.afi, &route.prefix_addr,
                                  route.prefix_len);
        isis_route_path_key_format(path_key, sizeof(path_key), route_key, &route);
        isis_route_head_table_add_path(desired, route_key, path_key, &route);

        pos += pfx_bytes + sub_extra;
    }
}

static void isis_spf_collect_prefixes_from_lsdb(isis_instance_cfg_t *inst, uint8_t level, GHashTable *nodes,
                                                GHashTable *desired)
{
    if (!inst || !inst->lsdb_entries || !nodes || !desired)
    {
        return;
    }

    const uint8_t want_narrow = (inst->cost_style == ISIS_COST_STYLE_NARROW) ? 1u : 0u;

    for (uint16_t afi = ROUTE_AFI_IPV4; afi <= ROUTE_AFI_IPV6; ++afi)
    {
        if ((afi == ROUTE_AFI_IPV4 && !inst->af_ipv4) || (afi == ROUTE_AFI_IPV6 && !inst->af_ipv6))
        {
            continue;
        }
        /* narrow (RFC 1195) 不支持 IPv6 */
        if (want_narrow && afi == ROUTE_AFI_IPV6)
        {
            continue;
        }

        GPtrArray *hops = g_ptr_array_new_with_free_func(isis_spf_local_hop_free);
        if (!hops)
        {
            continue;
        }

        isis_spf_collect_local_hops(inst, level, afi, hops);
        if (hops->len == 0u)
        {
            g_ptr_array_free(hops, TRUE);
            continue;
        }

        for (guint i = 0u; i < hops->len; ++i)
        {
            const isis_spf_local_hop_t *hop = (const isis_spf_local_hop_t *)g_ptr_array_index(hops, i);
            if (!hop)
            {
                continue;
            }

            isis_spf_run_dijkstra(nodes, hop->node_id);

            GHashTableIter iter;
            gpointer key = NULL;
            gpointer value = NULL;
            g_hash_table_iter_init(&iter, inst->lsdb_entries);
            while (g_hash_table_iter_next(&iter, &key, &value))
            {
                (void)key;
                const isis_lsdb_entry_t *entry = (const isis_lsdb_entry_t *)value;
                if (!entry || entry->level != level || !entry->tlvs || entry->tlvs->len == 0u)
                {
                    continue;
                }

                uint8_t entry_node_id[ISIS_SPF_NODE_ID_LEN];
                isis_spf_make_node_id(entry->system_id, entry->pseudonode_id, entry_node_id);

                uint64_t dist = 0u;
                if (memcmp(entry_node_id, hop->node_id, ISIS_SPF_NODE_ID_LEN) != 0 &&
                    !isis_spf_get_distance(nodes, entry_node_id, &dist))
                {
                    continue;
                }

                uint64_t path_metric =
                    (dist > (ISIS_SPF_INF_DIST - hop->local_metric)) ? ISIS_SPF_INF_DIST : (dist + hop->local_metric);

                const uint8_t *tlvs = entry->tlvs->data;
                size_t tlv_len = entry->tlvs->len;
                size_t pos = 0u;
                while (pos + 2u <= tlv_len)
                {
                    uint8_t tlv_type = tlvs[pos];
                    uint8_t len = tlvs[pos + 1u];
                    pos += 2u;
                    if (pos + len > tlv_len)
                    {
                        break;
                    }

                    if (afi == ROUTE_AFI_IPV4 && !want_narrow && tlv_type == ISIS_TLV_EXT_IP_REACH)
                    {
                        isis_spf_parse_prefix_entries(&tlvs[pos], len, ROUTE_AFI_IPV4, level, entry->system_id,
                                                      path_metric, hop, desired);
                    }
                    else if (afi == ROUTE_AFI_IPV4 && want_narrow && tlv_type == ISIS_TLV_IP_INT_REACH)
                    {
                        isis_spf_parse_narrow_ip_reach_entries(&tlvs[pos], len, level, entry->system_id, path_metric,
                                                               hop, desired);
                    }
                    else if (afi == ROUTE_AFI_IPV6 && !want_narrow && tlv_type == ISIS_TLV_IPV6_REACH)
                    {
                        isis_spf_parse_prefix_entries(&tlvs[pos], len, ROUTE_AFI_IPV6, level, entry->system_id,
                                                      path_metric, hop, desired);
                    }

                    pos += len;
                }
            }
        }

        g_ptr_array_free(hops, TRUE);
    }
}

static void isis_spf_reconcile_lsp_routes(isis_instance_cfg_t *inst, GHashTable *desired)
{
    if (!inst || !inst->learned_route_heads)
    {
        return;
    }
    (void)isis_route_reconcile_spf(inst, desired);
}

static void isis_spf_recompute_instance(isis_instance_cfg_t *inst)
{
    if (!inst || !inst->learned_route_heads)
    {
        return;
    }

    GHashTable *desired = isis_route_head_table_new();
    if (!desired)
    {
        return;
    }

    if (!inst->admin_up)
    {
        isis_spf_reconcile_lsp_routes(inst, desired);
        g_hash_table_destroy(desired);
        return;
    }

    uint8_t local_sysid[6] = {0};
    if (isis_extract_system_id(inst->net, local_sysid) != 0)
    {
        isis_spf_reconcile_lsp_routes(inst, desired);
        g_hash_table_destroy(desired);
        return;
    }

    for (uint8_t level = 1u; level <= 2u; ++level)
    {
        if (!isis_spf_level_enabled(inst, level))
        {
            continue;
        }

        GHashTable *nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_spf_node_free);
        if (!nodes)
        {
            continue;
        }

        uint8_t local_node_id[ISIS_SPF_NODE_ID_LEN];
        isis_spf_make_node_id(local_sysid, 0u, local_node_id);

        (void)isis_spf_graph_get_node(nodes, local_node_id, 1);
        isis_spf_add_root_edges(inst, level, local_sysid, nodes);
        isis_spf_collect_graph_from_lsdb(inst, level, nodes);
        isis_spf_run_dijkstra(nodes, local_node_id);

        if (inst->af_ipv4 || inst->af_ipv6)
        {
            isis_spf_collect_prefixes_from_lsdb(inst, level, nodes, desired);
        }

        g_hash_table_destroy(nodes);
    }

    isis_spf_reconcile_lsp_routes(inst, desired);
    g_hash_table_destroy(desired);
}

void isis_spf_process_lsp(isis_instance_cfg_t *inst, uint8_t level, const uint8_t origin_sysid[6],
                          const isis_neighbor_t *nexthop_nbr, const isis_if_cfg_t *if_cfg,
                          const if_api_cache_entry_t *if_entry, const uint8_t *tlvs, size_t tlv_len)
{
    (void)level;
    (void)origin_sysid;
    (void)nexthop_nbr;
    (void)if_cfg;
    (void)if_entry;
    (void)tlvs;
    (void)tlv_len;

    isis_spf_recompute_instance(inst);
}

void isis_spf_reconcile_instance(isis_instance_cfg_t *inst)
{
    isis_spf_recompute_instance(inst);
}

void isis_spf_withdraw_neighbor_routes(isis_instance_cfg_t *inst, const isis_neighbor_t *nbr)
{
    if (!nbr)
    {
        return;
    }
    isis_spf_withdraw_origin_routes(inst, nbr->level, nbr->system_id);
}

void isis_spf_withdraw_origin_routes(isis_instance_cfg_t *inst, uint8_t level, const uint8_t origin_sysid[6])
{
    if (!inst || !inst->learned_route_heads || !origin_sysid)
    {
        return;
    }

    char key_prefix[64] = {0};
    isis_spf_origin_prefix(key_prefix, sizeof(key_prefix), level, origin_sysid);

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->learned_route_heads);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        const char *route_key = (const char *)key;
        const isis_route_head_t *head = (const isis_route_head_t *)value;
        if (!route_key || !g_str_has_prefix(route_key, key_prefix))
        {
            continue;
        }

        const isis_route_path_t *best = isis_route_head_best_path(head);
        if (best)
        {
            (void)isis_route_sync_publish_del(&best->state);
        }
        g_hash_table_iter_remove(&iter);
    }
}
