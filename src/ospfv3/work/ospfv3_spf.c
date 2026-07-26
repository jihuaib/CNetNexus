/**
 * @file   ospfv3_spf.c
 * @brief  OSPFv3 intra-area SPF and IPv6 route calculation
 */
#include "ospfv3_spf.h"

#include <arpa/inet.h>
#include <limits.h>
#include <string.h>

#include "errcode.h"
#include "if.h"
#include "log.h"
#include "ospfv3_lsa.h"
#include "ospfv3_packet.h"
#include "ospfv3_route_sync.h"

#define OSPFV3_VERTEX_INFINITY UINT32_MAX
#define OSPFV3_ROUTER_LINK_POINT_TO_POINT 1u
#define OSPFV3_ROUTER_LINK_TRANSIT 2u

typedef struct ospfv3_spf_vertex
{
    char *key;
    uint32_t router_id;
    uint32_t network_id;
    uint32_t network_adv_router;
    uint32_t distance;
    uint32_t first_hop_router;
    uint8_t is_router;
    uint8_t visited;
} ospfv3_spf_vertex_t;

typedef struct ospfv3_spf_edge
{
    ospfv3_spf_vertex_t *from;
    ospfv3_spf_vertex_t *to;
    uint32_t cost;
} ospfv3_spf_edge_t;

typedef struct ospfv3_spf_prefix
{
    ospfv3_spf_vertex_t *owner;
    net_addr_t prefix;
    uint32_t advertising_router;
    uint32_t cost;
    uint8_t prefix_len;
} ospfv3_spf_prefix_t;

typedef struct ospfv3_spf_graph
{
    GHashTable *vertices;
    GPtrArray *vertex_list;
    GPtrArray *edges;
    GPtrArray *prefixes;
    uint32_t area_id;
} ospfv3_spf_graph_t;

static uint16_t ospfv3_get_u16(const uint8_t *p)
{
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return ntohs(value);
}

static uint32_t ospfv3_get_u32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}

static size_t ospfv3_prefix_wire_bytes(uint8_t prefix_len)
{
    return ((size_t)prefix_len + 31u) / 32u * 4u;
}

static uint32_t ospfv3_metric_add(uint32_t a, uint32_t b)
{
    return a == OSPFV3_VERTEX_INFINITY || b > OSPFV3_VERTEX_INFINITY - a ? OSPFV3_VERTEX_INFINITY : a + b;
}

static void ospfv3_spf_vertex_free(gpointer data)
{
    ospfv3_spf_vertex_t *vertex = data;
    if (vertex)
    {
        g_free(vertex->key);
        g_free(vertex);
    }
}

static ospfv3_spf_graph_t *ospfv3_spf_graph_create(uint32_t area_id)
{
    ospfv3_spf_graph_t *graph = g_malloc0(sizeof(*graph));
    if (!graph)
    {
        return NULL;
    }
    graph->area_id = area_id;
    graph->vertices = g_hash_table_new(g_str_hash, g_str_equal);
    graph->vertex_list = g_ptr_array_new_with_free_func(ospfv3_spf_vertex_free);
    graph->edges = g_ptr_array_new_with_free_func(g_free);
    graph->prefixes = g_ptr_array_new_with_free_func(g_free);
    if (!graph->vertices || !graph->vertex_list || !graph->edges || !graph->prefixes)
    {
        if (graph->vertices)
        {
            g_hash_table_destroy(graph->vertices);
        }
        if (graph->vertex_list)
        {
            g_ptr_array_free(graph->vertex_list, TRUE);
        }
        if (graph->edges)
        {
            g_ptr_array_free(graph->edges, TRUE);
        }
        if (graph->prefixes)
        {
            g_ptr_array_free(graph->prefixes, TRUE);
        }
        g_free(graph);
        return NULL;
    }
    return graph;
}

static void ospfv3_spf_graph_destroy(ospfv3_spf_graph_t *graph)
{
    if (!graph)
    {
        return;
    }
    g_hash_table_destroy(graph->vertices);
    g_ptr_array_free(graph->vertex_list, TRUE);
    g_ptr_array_free(graph->edges, TRUE);
    g_ptr_array_free(graph->prefixes, TRUE);
    g_free(graph);
}

static ospfv3_spf_vertex_t *ospfv3_spf_router_get(ospfv3_spf_graph_t *graph, uint32_t router_id)
{
    char key[32];
    g_snprintf(key, sizeof(key), "R:%08x", router_id);
    ospfv3_spf_vertex_t *vertex = g_hash_table_lookup(graph->vertices, key);
    if (vertex)
    {
        return vertex;
    }
    vertex = g_malloc0(sizeof(*vertex));
    if (!vertex)
    {
        return NULL;
    }
    vertex->key = g_strdup(key);
    vertex->is_router = 1u;
    vertex->router_id = router_id;
    vertex->distance = OSPFV3_VERTEX_INFINITY;
    g_hash_table_insert(graph->vertices, vertex->key, vertex);
    g_ptr_array_add(graph->vertex_list, vertex);
    return vertex;
}

static ospfv3_spf_vertex_t *ospfv3_spf_network_get(ospfv3_spf_graph_t *graph, uint32_t adv_router,
                                                   uint32_t interface_id)
{
    char key[48];
    g_snprintf(key, sizeof(key), "N:%08x:%08x", adv_router, interface_id);
    ospfv3_spf_vertex_t *vertex = g_hash_table_lookup(graph->vertices, key);
    if (vertex)
    {
        return vertex;
    }
    vertex = g_malloc0(sizeof(*vertex));
    if (!vertex)
    {
        return NULL;
    }
    vertex->key = g_strdup(key);
    vertex->network_adv_router = adv_router;
    vertex->network_id = interface_id;
    vertex->distance = OSPFV3_VERTEX_INFINITY;
    g_hash_table_insert(graph->vertices, vertex->key, vertex);
    g_ptr_array_add(graph->vertex_list, vertex);
    return vertex;
}

static void ospfv3_spf_add_edge(ospfv3_spf_graph_t *graph, ospfv3_spf_vertex_t *from, ospfv3_spf_vertex_t *to,
                                uint32_t cost)
{
    if (!from || !to)
    {
        return;
    }
    ospfv3_spf_edge_t *edge = g_malloc0(sizeof(*edge));
    if (edge)
    {
        edge->from = from;
        edge->to = to;
        edge->cost = cost;
        g_ptr_array_add(graph->edges, edge);
    }
}

static const ospfv3_lsa_entry_t *ospfv3_find_router_lsa(const ospfv3_instance_t *inst, uint32_t area_id,
                                                        uint32_t router_id, uint64_t now_msec)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_lsa_entry_t *entry = value;
        if (entry->area_id == area_id && entry->type == OSPFV3_LSA_ROUTER && entry->advertising_router == router_id &&
            ospfv3_lsa_current_age(entry, now_msec) < OSPFV3_LSA_MAX_AGE)
        {
            return entry;
        }
    }
    return NULL;
}

static gboolean ospfv3_router_has_p2p_backlink(const ospfv3_instance_t *inst, uint32_t area_id, uint32_t router_id,
                                               uint32_t peer_router_id, uint64_t now_msec)
{
    const ospfv3_lsa_entry_t *entry = ospfv3_find_router_lsa(inst, area_id, router_id, now_msec);
    if (!entry || !entry->raw || entry->raw->len < OSPFV3_LSA_HEADER_LEN + 4u)
    {
        return FALSE;
    }
    const uint8_t *body = entry->raw->data + OSPFV3_LSA_HEADER_LEN;
    size_t body_len = entry->raw->len - OSPFV3_LSA_HEADER_LEN;
    for (size_t offset = 4u; offset + 16u <= body_len; offset += 16u)
    {
        if (body[offset] == OSPFV3_ROUTER_LINK_POINT_TO_POINT && ospfv3_get_u32(body + offset + 12u) == peer_router_id)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean ospfv3_network_has_router(const ospfv3_lsa_entry_t *entry, uint32_t router_id)
{
    if (!entry || !entry->raw || entry->raw->len < OSPFV3_LSA_HEADER_LEN + 8u)
    {
        return FALSE;
    }
    const uint8_t *body = entry->raw->data + OSPFV3_LSA_HEADER_LEN;
    size_t body_len = entry->raw->len - OSPFV3_LSA_HEADER_LEN;
    for (size_t offset = 4u; offset + 4u <= body_len; offset += 4u)
    {
        if (ospfv3_get_u32(body + offset) == router_id)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void ospfv3_spf_parse_router_lsa(ospfv3_spf_graph_t *graph, const ospfv3_instance_t *inst,
                                        const ospfv3_lsa_entry_t *entry, uint64_t now_msec)
{
    const uint8_t *body = entry->raw->data + OSPFV3_LSA_HEADER_LEN;
    size_t body_len = entry->raw->len - OSPFV3_LSA_HEADER_LEN;
    ospfv3_spf_vertex_t *router = ospfv3_spf_router_get(graph, entry->advertising_router);
    for (size_t offset = 4u; offset + 16u <= body_len; offset += 16u)
    {
        uint8_t type = body[offset];
        uint16_t metric = ospfv3_get_u16(body + offset + 2u);
        uint32_t nbr_ifid = ospfv3_get_u32(body + offset + 8u);
        uint32_t nbr_router = ospfv3_get_u32(body + offset + 12u);
        if (type == OSPFV3_ROUTER_LINK_POINT_TO_POINT &&
            ospfv3_router_has_p2p_backlink(inst, graph->area_id, nbr_router, entry->advertising_router, now_msec))
        {
            ospfv3_spf_add_edge(graph, router, ospfv3_spf_router_get(graph, nbr_router), metric);
        }
        else if (type == OSPFV3_ROUTER_LINK_TRANSIT)
        {
            const ospfv3_lsa_entry_t *network =
                ospfv3_lsa_lookup(inst, graph->area_id, OSPFV3_LSA_NETWORK, nbr_ifid, nbr_router);
            if (network && ospfv3_network_has_router(network, entry->advertising_router))
            {
                ospfv3_spf_add_edge(graph, router, ospfv3_spf_network_get(graph, nbr_router, nbr_ifid), metric);
            }
        }
    }
}

static void ospfv3_spf_parse_network_lsa(ospfv3_spf_graph_t *graph, const ospfv3_instance_t *inst,
                                         const ospfv3_lsa_entry_t *entry, uint64_t now_msec)
{
    ospfv3_spf_vertex_t *network = ospfv3_spf_network_get(graph, entry->advertising_router, entry->link_state_id);
    const uint8_t *body = entry->raw->data + OSPFV3_LSA_HEADER_LEN;
    size_t body_len = entry->raw->len - OSPFV3_LSA_HEADER_LEN;
    for (size_t offset = 4u; offset + 4u <= body_len; offset += 4u)
    {
        uint32_t router_id = ospfv3_get_u32(body + offset);
        if (ospfv3_find_router_lsa(inst, graph->area_id, router_id, now_msec))
        {
            ospfv3_spf_add_edge(graph, network, ospfv3_spf_router_get(graph, router_id), 0u);
        }
    }
}

static void ospfv3_spf_parse_intra_prefix_lsa(ospfv3_spf_graph_t *graph, const ospfv3_lsa_entry_t *entry)
{
    const uint8_t *body = entry->raw->data + OSPFV3_LSA_HEADER_LEN;
    size_t body_len = entry->raw->len - OSPFV3_LSA_HEADER_LEN;
    uint16_t count = ospfv3_get_u16(body);
    uint16_t ref_type = ospfv3_get_u16(body + 2u);
    uint32_t ref_id = ospfv3_get_u32(body + 4u);
    uint32_t ref_adv = ospfv3_get_u32(body + 8u);
    ospfv3_spf_vertex_t *owner =
        ref_type == OSPFV3_LSA_ROUTER
            ? ospfv3_spf_router_get(graph, ref_adv)
            : (ref_type == OSPFV3_LSA_NETWORK ? ospfv3_spf_network_get(graph, ref_adv, ref_id) : NULL);
    size_t offset = 12u;
    for (uint16_t i = 0u; owner && i < count && offset + 4u <= body_len; ++i)
    {
        uint8_t prefix_len = body[offset];
        size_t bytes = ospfv3_prefix_wire_bytes(prefix_len);
        if (prefix_len > 128u || offset + 4u + bytes > body_len)
        {
            break;
        }
        ospfv3_spf_prefix_t *prefix = g_malloc0(sizeof(*prefix));
        if (prefix)
        {
            prefix->owner = owner;
            prefix->prefix.family = AF_INET6;
            memcpy(prefix->prefix.u.v6.s6_addr, body + offset + 4u, bytes);
            prefix->prefix_len = prefix_len;
            prefix->cost = ospfv3_get_u16(body + offset + 2u);
            prefix->advertising_router = entry->advertising_router;
            g_ptr_array_add(graph->prefixes, prefix);
        }
        offset += 4u + bytes;
    }
}

static ospfv3_spf_graph_t *ospfv3_spf_build_graph(const ospfv3_instance_t *inst, uint32_t area_id, uint64_t now_msec)
{
    ospfv3_spf_graph_t *graph = ospfv3_spf_graph_create(area_id);
    if (!graph)
    {
        return NULL;
    }
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_lsa_entry_t *entry = value;
        if (entry->area_id != area_id || !entry->raw || ospfv3_lsa_current_age(entry, now_msec) >= OSPFV3_LSA_MAX_AGE)
        {
            continue;
        }
        if (entry->type == OSPFV3_LSA_ROUTER)
        {
            ospfv3_spf_parse_router_lsa(graph, inst, entry, now_msec);
        }
        else if (entry->type == OSPFV3_LSA_NETWORK)
        {
            ospfv3_spf_parse_network_lsa(graph, inst, entry, now_msec);
        }
    }
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_lsa_entry_t *entry = value;
        if (entry->area_id == area_id && entry->type == OSPFV3_LSA_INTRA_AREA_PREFIX && entry->raw &&
            ospfv3_lsa_current_age(entry, now_msec) < OSPFV3_LSA_MAX_AGE)
        {
            ospfv3_spf_parse_intra_prefix_lsa(graph, entry);
        }
    }
    return graph;
}

static void ospfv3_spf_run(ospfv3_spf_graph_t *graph, uint32_t root_router)
{
    ospfv3_spf_vertex_t *root = ospfv3_spf_router_get(graph, root_router);
    root->distance = 0u;
    for (;;)
    {
        ospfv3_spf_vertex_t *best = NULL;
        for (guint i = 0u; i < graph->vertex_list->len; ++i)
        {
            ospfv3_spf_vertex_t *vertex = g_ptr_array_index(graph->vertex_list, i);
            if (!vertex->visited && vertex->distance != OSPFV3_VERTEX_INFINITY &&
                (!best || vertex->distance < best->distance))
            {
                best = vertex;
            }
        }
        if (!best)
        {
            break;
        }
        best->visited = 1u;
        for (guint i = 0u; i < graph->edges->len; ++i)
        {
            ospfv3_spf_edge_t *edge = g_ptr_array_index(graph->edges, i);
            if (edge->from != best)
            {
                continue;
            }
            uint32_t candidate = ospfv3_metric_add(best->distance, edge->cost);
            uint32_t first_hop =
                best == root ? (edge->to->is_router ? edge->to->router_id : 0u) : best->first_hop_router;
            if (first_hop == 0u && edge->to->is_router)
            {
                first_hop = edge->to->router_id;
            }
            if (candidate < edge->to->distance)
            {
                edge->to->distance = candidate;
                edge->to->first_hop_router = first_hop;
            }
        }
    }
}

static ospfv3_neighbor_t *ospfv3_spf_find_neighbor(ospfv3_instance_t *inst, uint32_t area_id, uint32_t router_id)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        ospfv3_neighbor_t *nbr = value;
        if (nbr->area_id == area_id && nbr->router_id == router_id && nbr->state == OSPFV3_NBR_STATE_FULL)
        {
            return nbr;
        }
    }
    return NULL;
}

static gboolean ospfv3_spf_is_direct_prefix(const ospfv3_instance_t *inst, const net_addr_t *prefix, uint8_t prefix_len)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_if_cfg_t *cfg = value;
        const if_api_cache_entry_t *entry = if_api_cache_lookup(cfg->ifname);
        if (!cfg->enabled || !ospfv3_if_entry_matches_vrf(inst->vrf_name, entry) || entry->ipv6_addr.family != AF_INET6)
        {
            continue;
        }
        net_addr_t local = entry->ipv6_addr;
        uint8_t local_len = g_str_has_prefix(cfg->ifname, "loop") ? 128u : entry->ipv6_prefix_len;
        if (local_len == prefix_len && net_addr_prefix_normalize(&local, local_len) == 0 &&
            net_addr_equal(&local, prefix))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static char *ospfv3_route_key_new(const net_addr_t *prefix, uint8_t prefix_len)
{
    char text[INET6_ADDRSTRLEN];
    if (!inet_ntop(AF_INET6, &prefix->u.v6, text, sizeof(text)))
    {
        return NULL;
    }
    return g_strdup_printf("%s/%u", text, prefix_len);
}

static void ospfv3_spf_add_desired_route(ospfv3_instance_t *inst, ospfv3_spf_graph_t *graph,
                                         const ospfv3_spf_prefix_t *item, GHashTable *desired)
{
    if (!item->owner || item->owner->distance == OSPFV3_VERTEX_INFINITY ||
        ospfv3_spf_is_direct_prefix(inst, &item->prefix, item->prefix_len))
    {
        return;
    }
    uint32_t first_hop = item->owner->first_hop_router;
    ospfv3_neighbor_t *nbr = ospfv3_spf_find_neighbor(inst, graph->area_id, first_hop);
    if (!nbr)
    {
        return;
    }
    const if_api_cache_entry_t *entry = if_api_cache_lookup(nbr->ifname);
    if (!ospfv3_if_entry_matches_vrf(inst->vrf_name, entry) || entry->ifindex == 0u)
    {
        return;
    }

    ospfv3_route_t *route = g_malloc0(sizeof(*route));
    char *key = ospfv3_route_key_new(&item->prefix, item->prefix_len);
    if (!route || !key)
    {
        g_free(route);
        g_free(key);
        return;
    }
    route->prefix = item->prefix;
    route->prefix_len = item->prefix_len;
    route->source.family = AF_INET;
    route->source.u.v4.s_addr = htonl(item->advertising_router);
    route->nexthop.family = AF_INET6;
    route->nexthop.u.v6 = nbr->src_addr;
    route->out_ifindex = entry->ifindex;
    route->metric = ospfv3_metric_add(item->owner->distance, item->cost);
    route->advertising_router = item->advertising_router;
    route->vrf_id = inst->vrf_id;

    ospfv3_route_t *old = g_hash_table_lookup(desired, key);
    if (!old || route->metric < old->metric)
    {
        g_hash_table_replace(desired, key, route);
    }
    else
    {
        g_free(key);
        g_free(route);
    }
}

static int ospfv3_route_equal(const ospfv3_route_t *a, const ospfv3_route_t *b)
{
    return a && b && a->prefix_len == b->prefix_len && a->out_ifindex == b->out_ifindex && a->metric == b->metric &&
           a->advertising_router == b->advertising_router && net_addr_equal(&a->prefix, &b->prefix) &&
           net_addr_equal(&a->nexthop, &b->nexthop);
}

static void ospfv3_spf_reconcile_routes(ospfv3_instance_t *inst, GHashTable *desired)
{
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->routes);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        ospfv3_route_t *current = value;
        ospfv3_route_t *next = g_hash_table_lookup(desired, key);
        if (!next)
        {
            (void)ospfv3_route_sync_enqueue_del(current);
            g_hash_table_iter_remove(&iter);
        }
        else if (!ospfv3_route_equal(current, next))
        {
            (void)ospfv3_route_sync_enqueue_replace(current, next);
            g_hash_table_iter_replace(&iter, g_memdup2(next, sizeof(*next)));
        }
        g_hash_table_remove(desired, key);
    }
    g_hash_table_iter_init(&iter, desired);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        ospfv3_route_t *route = value;
        if (ospfv3_route_sync_enqueue_add(route) == ERRCODE_SUCCESS)
        {
            g_hash_table_insert(inst->routes, g_strdup(key), g_memdup2(route, sizeof(*route)));
        }
    }
}

void ospfv3_spf_recalculate(ospfv3_instance_t *inst)
{
    if (!inst || inst->router_id == 0u)
    {
        return;
    }
    GHashTable *desired = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!desired)
    {
        return;
    }
    GHashTableIter area_iter;
    gpointer area_key = NULL;
    g_hash_table_iter_init(&area_iter, inst->areas);
    while (g_hash_table_iter_next(&area_iter, &area_key, NULL))
    {
        uint32_t area_id = *(uint32_t *)area_key;
        ospfv3_spf_graph_t *graph = ospfv3_spf_build_graph(inst, area_id, ospfv3_now_msec());
        if (!graph)
        {
            continue;
        }
        ospfv3_spf_run(graph, inst->router_id);
        for (guint i = 0u; i < graph->prefixes->len; ++i)
        {
            ospfv3_spf_add_desired_route(inst, graph, g_ptr_array_index(graph->prefixes, i), desired);
        }
        ospfv3_spf_graph_destroy(graph);
    }
    ospfv3_spf_reconcile_routes(inst, desired);
    g_hash_table_destroy(desired);
}

static void ospfv3_spf_recalculate_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    (void)user_data;
    ospfv3_spf_recalculate(value);
}

void ospfv3_spf_recalculate_all(void)
{
    if (g_ospfv3_work_local && g_ospfv3_work_local->instances)
    {
        g_hash_table_foreach(g_ospfv3_work_local->instances, ospfv3_spf_recalculate_cb, NULL);
    }
}
