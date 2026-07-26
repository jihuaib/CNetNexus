/**
 * @file   ospf_spf.c
 * @brief  OSPFv2 intra-area shortest-path calculation
 */
#include "ospf_spf.h"

#include <arpa/inet.h>
#include <limits.h>
#include <string.h>

#include "errcode.h"
#include "if.h"
#include "log.h"
#include "ospf_lsa.h"
#include "ospf_packet.h"
#include "ospf_route_sync.h"

#define OSPF_VERTEX_INFINITY UINT32_MAX
#define OSPF_ROUTER_LINK_POINT_TO_POINT 1u
#define OSPF_ROUTER_LINK_TRANSIT 2u
#define OSPF_ROUTER_LINK_STUB 3u

typedef struct ospf_spf_vertex
{
    char *key;
    uint32_t id;
    uint32_t distance;
    uint32_t first_hop_router;
    uint8_t is_router;
    uint8_t visited;
} ospf_spf_vertex_t;

typedef struct ospf_spf_edge
{
    ospf_spf_vertex_t *from;
    ospf_spf_vertex_t *to;
    uint32_t cost;
} ospf_spf_edge_t;

typedef struct ospf_spf_prefix
{
    ospf_spf_vertex_t *owner;
    uint32_t prefix;
    uint32_t advertising_router;
    uint32_t cost;
    uint8_t prefix_len;
} ospf_spf_prefix_t;

typedef struct ospf_spf_graph
{
    GHashTable *vertices;
    GPtrArray *vertex_list;
    GPtrArray *edges;
    GPtrArray *prefixes;
    uint32_t area_id;
} ospf_spf_graph_t;

static uint16_t ospf_get_u16(const uint8_t *p)
{
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return ntohs(value);
}

static uint32_t ospf_get_u32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}

static uint32_t ospf_metric_add(uint32_t a, uint32_t b)
{
    if (a == OSPF_VERTEX_INFINITY || b > OSPF_VERTEX_INFINITY - a)
    {
        return OSPF_VERTEX_INFINITY;
    }
    return a + b;
}

static void ospf_spf_vertex_free(gpointer data)
{
    ospf_spf_vertex_t *vertex = (ospf_spf_vertex_t *)data;
    if (vertex)
    {
        g_free(vertex->key);
        g_free(vertex);
    }
}

static ospf_spf_graph_t *ospf_spf_graph_create(uint32_t area_id)
{
    ospf_spf_graph_t *graph = g_malloc0(sizeof(*graph));
    if (!graph)
    {
        return NULL;
    }
    graph->area_id = area_id;
    graph->vertices = g_hash_table_new(g_str_hash, g_str_equal);
    graph->vertex_list = g_ptr_array_new();
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

static void ospf_spf_graph_destroy(ospf_spf_graph_t *graph)
{
    if (!graph)
    {
        return;
    }
    if (graph->vertices)
    {
        g_hash_table_destroy(graph->vertices);
    }
    if (graph->vertex_list)
    {
        for (guint i = 0u; i < graph->vertex_list->len; ++i)
        {
            ospf_spf_vertex_free(g_ptr_array_index(graph->vertex_list, i));
        }
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
}

static ospf_spf_vertex_t *ospf_spf_vertex_get(ospf_spf_graph_t *graph, int is_router, uint32_t id)
{
    char key[32];
    g_snprintf(key, sizeof(key), "%c:%08x", is_router ? 'R' : 'N', id);
    ospf_spf_vertex_t *vertex = g_hash_table_lookup(graph->vertices, key);
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
    if (!vertex->key)
    {
        g_free(vertex);
        return NULL;
    }
    vertex->id = id;
    vertex->is_router = is_router ? 1u : 0u;
    vertex->distance = OSPF_VERTEX_INFINITY;
    g_hash_table_insert(graph->vertices, vertex->key, vertex);
    g_ptr_array_add(graph->vertex_list, vertex);
    return vertex;
}

static void ospf_spf_add_edge(ospf_spf_graph_t *graph, ospf_spf_vertex_t *from, ospf_spf_vertex_t *to, uint32_t cost)
{
    if (!from || !to)
    {
        return;
    }
    ospf_spf_edge_t *edge = g_malloc0(sizeof(*edge));
    if (!edge)
    {
        return;
    }
    edge->from = from;
    edge->to = to;
    edge->cost = cost;
    g_ptr_array_add(graph->edges, edge);
}

static uint8_t ospf_mask_prefix_len(uint32_t mask)
{
    uint8_t prefix_len = 0u;
    gboolean saw_zero = FALSE;
    for (int bit = 31; bit >= 0; --bit)
    {
        gboolean set = (mask & (1u << bit)) != 0u;
        if (set && saw_zero)
        {
            return 255u;
        }
        if (set)
        {
            prefix_len++;
        }
        else
        {
            saw_zero = TRUE;
        }
    }
    return prefix_len;
}

static void ospf_spf_add_prefix(ospf_spf_graph_t *graph, ospf_spf_vertex_t *owner, uint32_t prefix, uint32_t mask,
                                uint32_t cost, uint32_t advertising_router)
{
    uint8_t prefix_len = ospf_mask_prefix_len(mask);
    if (!owner || prefix_len > 32u)
    {
        return;
    }
    ospf_spf_prefix_t *item = g_malloc0(sizeof(*item));
    if (!item)
    {
        return;
    }
    item->owner = owner;
    item->prefix = prefix & mask;
    item->prefix_len = prefix_len;
    item->cost = cost;
    item->advertising_router = advertising_router;
    g_ptr_array_add(graph->prefixes, item);
}

static gboolean ospf_spf_router_has_p2p_link(const ospf_lsa_entry_t *entry, uint32_t peer_router_id, uint64_t now_msec)
{
    if (!entry || !entry->raw || entry->raw->len < OSPF_LSA_HEADER_LEN + 4u ||
        ospf_lsa_current_age(entry, now_msec) >= OSPF_LSA_MAX_AGE)
    {
        return FALSE;
    }

    const uint8_t *body = entry->raw->data + OSPF_LSA_HEADER_LEN;
    size_t body_len = entry->raw->len - OSPF_LSA_HEADER_LEN;
    uint16_t link_count = ospf_get_u16(body + 2u);
    size_t offset = 4u;
    for (uint16_t i = 0u; i < link_count; ++i)
    {
        if (offset + 12u > body_len)
        {
            return FALSE;
        }
        uint8_t tos_count = body[offset + 9u];
        size_t link_len = 12u + ((size_t)tos_count * 4u);
        if (link_len > body_len - offset)
        {
            return FALSE;
        }
        if (body[offset + 8u] == OSPF_ROUTER_LINK_POINT_TO_POINT && ospf_get_u32(body + offset) == peer_router_id)
        {
            return TRUE;
        }
        offset += link_len;
    }
    return FALSE;
}

static gboolean ospf_spf_router_has_transit_link(const ospf_lsa_entry_t *entry, uint32_t network_id, uint64_t now_msec)
{
    if (!entry || !entry->raw || entry->raw->len < OSPF_LSA_HEADER_LEN + 4u ||
        ospf_lsa_current_age(entry, now_msec) >= OSPF_LSA_MAX_AGE)
    {
        return FALSE;
    }

    const uint8_t *body = entry->raw->data + OSPF_LSA_HEADER_LEN;
    size_t body_len = entry->raw->len - OSPF_LSA_HEADER_LEN;
    uint16_t link_count = ospf_get_u16(body + 2u);
    size_t offset = 4u;
    for (uint16_t i = 0u; i < link_count; ++i)
    {
        if (offset + 12u > body_len)
        {
            return FALSE;
        }
        uint8_t tos_count = body[offset + 9u];
        size_t link_len = 12u + ((size_t)tos_count * 4u);
        if (link_len > body_len - offset)
        {
            return FALSE;
        }
        if (body[offset + 8u] == OSPF_ROUTER_LINK_TRANSIT && ospf_get_u32(body + offset) == network_id)
        {
            return TRUE;
        }
        offset += link_len;
    }
    return FALSE;
}

static gboolean ospf_spf_network_has_router(const ospf_instance_t *inst, uint32_t area_id, uint32_t network_id,
                                            uint32_t router_id, uint64_t now_msec)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_lsa_entry_t *entry = (const ospf_lsa_entry_t *)value;
        if (entry->area_id != area_id || entry->type != OSPF_LSA_NETWORK || entry->link_state_id != network_id ||
            ospf_lsa_current_age(entry, now_msec) >= OSPF_LSA_MAX_AGE || !entry->raw ||
            entry->raw->len < OSPF_LSA_HEADER_LEN + 8u)
        {
            continue;
        }

        const uint8_t *body = entry->raw->data + OSPF_LSA_HEADER_LEN;
        size_t body_len = entry->raw->len - OSPF_LSA_HEADER_LEN;
        if (((body_len - 4u) % 4u) != 0u)
        {
            return FALSE;
        }
        for (size_t offset = 4u; offset + 4u <= body_len; offset += 4u)
        {
            if (ospf_get_u32(body + offset) == router_id)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static gboolean ospf_spf_p2p_link_is_bidirectional(const ospf_instance_t *inst, uint32_t area_id,
                                                   uint32_t local_router_id, uint32_t peer_router_id, uint64_t now_msec)
{
    const ospf_lsa_entry_t *peer = ospf_lsa_lookup(inst, area_id, OSPF_LSA_ROUTER, peer_router_id, peer_router_id);
    return ospf_spf_router_has_p2p_link(peer, local_router_id, now_msec);
}

static void ospf_spf_parse_router_lsa(ospf_spf_graph_t *graph, const ospf_instance_t *inst,
                                      const ospf_lsa_entry_t *entry, uint64_t now_msec)
{
    if (!entry->raw || entry->raw->len < OSPF_LSA_HEADER_LEN + 4u)
    {
        return;
    }
    const uint8_t *body = entry->raw->data + OSPF_LSA_HEADER_LEN;
    size_t body_len = entry->raw->len - OSPF_LSA_HEADER_LEN;
    uint16_t link_count = ospf_get_u16(body + 2u);
    size_t offset = 4u;
    ospf_spf_vertex_t *router = ospf_spf_vertex_get(graph, 1, entry->advertising_router);
    if (!router)
    {
        return;
    }

    for (uint16_t i = 0u; i < link_count; ++i)
    {
        if (offset + 12u > body_len)
        {
            return;
        }
        uint32_t link_id = ospf_get_u32(body + offset);
        uint32_t link_data = ospf_get_u32(body + offset + 4u);
        uint8_t link_type = body[offset + 8u];
        uint8_t tos_count = body[offset + 9u];
        uint16_t metric = ospf_get_u16(body + offset + 10u);
        size_t link_len = 12u + ((size_t)tos_count * 4u);
        if (link_len > body_len - offset)
        {
            return;
        }

        if (link_type == OSPF_ROUTER_LINK_POINT_TO_POINT)
        {
            if (ospf_spf_p2p_link_is_bidirectional(inst, graph->area_id, entry->advertising_router, link_id, now_msec))
            {
                ospf_spf_add_edge(graph, router, ospf_spf_vertex_get(graph, 1, link_id), metric);
            }
        }
        else if (link_type == OSPF_ROUTER_LINK_TRANSIT)
        {
            if (ospf_spf_network_has_router(inst, graph->area_id, link_id, entry->advertising_router, now_msec))
            {
                ospf_spf_add_edge(graph, router, ospf_spf_vertex_get(graph, 0, link_id), metric);
            }
        }
        else if (link_type == OSPF_ROUTER_LINK_STUB)
        {
            ospf_spf_add_prefix(graph, router, link_id, link_data, metric, entry->advertising_router);
        }

        offset += link_len;
    }
}

static void ospf_spf_parse_network_lsa(ospf_spf_graph_t *graph, const ospf_instance_t *inst,
                                       const ospf_lsa_entry_t *entry, uint64_t now_msec)
{
    if (!entry->raw || entry->raw->len < OSPF_LSA_HEADER_LEN + 8u)
    {
        return;
    }
    const uint8_t *body = entry->raw->data + OSPF_LSA_HEADER_LEN;
    size_t body_len = entry->raw->len - OSPF_LSA_HEADER_LEN;
    if (((body_len - 4u) % 4u) != 0u)
    {
        return;
    }

    ospf_spf_vertex_t *network = ospf_spf_vertex_get(graph, 0, entry->link_state_id);
    if (!network)
    {
        return;
    }
    uint32_t mask = ospf_get_u32(body);
    ospf_spf_add_prefix(graph, network, entry->link_state_id, mask, 0u, entry->advertising_router);
    for (size_t offset = 4u; offset + 4u <= body_len; offset += 4u)
    {
        uint32_t router_id = ospf_get_u32(body + offset);
        const ospf_lsa_entry_t *router = ospf_lsa_lookup(inst, graph->area_id, OSPF_LSA_ROUTER, router_id, router_id);
        if (ospf_spf_router_has_transit_link(router, entry->link_state_id, now_msec))
        {
            ospf_spf_add_edge(graph, network, ospf_spf_vertex_get(graph, 1, router_id), 0u);
        }
    }
}

static ospf_spf_graph_t *ospf_spf_build_graph(const ospf_instance_t *inst, uint32_t area_id, uint64_t now_msec)
{
    ospf_spf_graph_t *graph = ospf_spf_graph_create(area_id);
    if (!graph)
    {
        return NULL;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_lsa_entry_t *entry = (const ospf_lsa_entry_t *)value;
        if (entry->area_id != area_id || ospf_lsa_current_age(entry, now_msec) >= OSPF_LSA_MAX_AGE)
        {
            continue;
        }
        if (entry->type == OSPF_LSA_ROUTER)
        {
            ospf_spf_parse_router_lsa(graph, inst, entry, now_msec);
        }
        else if (entry->type == OSPF_LSA_NETWORK)
        {
            ospf_spf_parse_network_lsa(graph, inst, entry, now_msec);
        }
    }
    return graph;
}

static void ospf_spf_run(ospf_spf_graph_t *graph, uint32_t root_router)
{
    ospf_spf_vertex_t *root = ospf_spf_vertex_get(graph, 1, root_router);
    if (!root)
    {
        return;
    }
    root->distance = 0u;
    root->first_hop_router = 0u;

    for (;;)
    {
        ospf_spf_vertex_t *best = NULL;
        for (guint i = 0u; i < graph->vertex_list->len; ++i)
        {
            ospf_spf_vertex_t *vertex = g_ptr_array_index(graph->vertex_list, i);
            if (!vertex->visited && vertex->distance != OSPF_VERTEX_INFINITY &&
                (!best || vertex->distance < best->distance ||
                 (vertex->distance == best->distance && strcmp(vertex->key, best->key) < 0)))
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
            const ospf_spf_edge_t *edge = g_ptr_array_index(graph->edges, i);
            if (edge->from != best || edge->to->visited)
            {
                continue;
            }
            uint32_t candidate = ospf_metric_add(best->distance, edge->cost);
            uint32_t first_hop = best->first_hop_router;
            if (best == root && edge->to->is_router)
            {
                first_hop = edge->to->id;
            }
            else if (first_hop == 0u && !best->is_router && edge->to->is_router)
            {
                first_hop = edge->to->id;
            }

            if (candidate < edge->to->distance ||
                (candidate == edge->to->distance && first_hop != 0u &&
                 (edge->to->first_hop_router == 0u || first_hop < edge->to->first_hop_router)))
            {
                edge->to->distance = candidate;
                edge->to->first_hop_router = first_hop;
            }
        }
    }
}

static ospf_neighbor_t *ospf_spf_find_neighbor(ospf_instance_t *inst, uint32_t area_id, uint32_t router_id)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        ospf_neighbor_t *nbr = (ospf_neighbor_t *)value;
        if (nbr->state == OSPF_NBR_STATE_FULL && nbr->area_id == area_id && nbr->router_id == router_id)
        {
            return nbr;
        }
    }
    return NULL;
}

static gboolean ospf_spf_is_direct_prefix(const ospf_instance_t *inst, uint32_t prefix, uint8_t prefix_len)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_if_cfg_t *cfg = (const ospf_if_cfg_t *)value;
        const if_api_cache_entry_t *if_entry = if_api_cache_lookup(cfg->ifname);
        if (!cfg->enabled || !ospf_if_entry_matches_vrf(inst->vrf_name, if_entry) || !if_entry->link_up ||
            !if_entry->proto_up || if_entry->ifindex == 0u || if_entry->ipv4_addr.family != AF_INET)
        {
            continue;
        }
        uint8_t local_len = g_str_has_prefix(cfg->ifname, "loop") ? 32u : if_entry->ipv4_prefix_len;
        uint32_t mask = (local_len == 0u) ? 0u : (local_len == 32u ? 0xffffffffu : 0xffffffffu << (32u - local_len));
        uint32_t local_prefix = ntohl(if_entry->ipv4_addr.u.v4.s_addr) & mask;
        if (local_len == prefix_len && local_prefix == prefix)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static char *ospf_route_key_new(uint32_t prefix, uint8_t prefix_len)
{
    struct in_addr addr = {.s_addr = htonl(prefix)};
    char text[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr, text, sizeof(text)))
    {
        return NULL;
    }
    return g_strdup_printf("%s/%u", text, (unsigned)prefix_len);
}

static void ospf_spf_add_desired_route(ospf_instance_t *inst, ospf_spf_graph_t *graph, const ospf_spf_prefix_t *item,
                                       GHashTable *desired)
{
    if (!item->owner || item->owner->distance == OSPF_VERTEX_INFINITY ||
        ospf_spf_is_direct_prefix(inst, item->prefix, item->prefix_len))
    {
        return;
    }

    uint32_t first_hop = item->owner->first_hop_router;
    if (first_hop == 0u || item->advertising_router == inst->router_id)
    {
        return;
    }
    ospf_neighbor_t *nbr = ospf_spf_find_neighbor(inst, graph->area_id, first_hop);
    const if_api_cache_entry_t *if_entry = nbr ? if_api_cache_lookup(nbr->ifname) : NULL;
    if (!nbr || !ospf_if_entry_matches_vrf(inst->vrf_name, if_entry) || !if_entry->link_up || !if_entry->proto_up ||
        if_entry->ifindex == 0u || if_entry->ipv4_addr.family != AF_INET)
    {
        return;
    }

    ospf_route_t *route = g_malloc0(sizeof(*route));
    char *key = ospf_route_key_new(item->prefix, item->prefix_len);
    if (!route || !key)
    {
        g_free(route);
        g_free(key);
        return;
    }
    route->prefix.family = AF_INET;
    route->prefix.u.v4.s_addr = htonl(item->prefix);
    route->prefix_len = item->prefix_len;
    route->source.family = AF_INET;
    route->source.u.v4.s_addr = htonl(inst->router_id);
    route->nexthop.family = AF_INET;
    route->nexthop.u.v4.s_addr = htonl(nbr->src_addr);
    route->out_ifindex = if_entry->ifindex;
    route->metric = ospf_metric_add(item->owner->distance, item->cost);
    route->advertising_router = item->advertising_router;
    route->vrf_id = inst->vrf_id;

    ospf_route_t *current = g_hash_table_lookup(desired, key);
    if (current &&
        (current->metric < route->metric || (current->metric == route->metric &&
                                             ntohl(current->nexthop.u.v4.s_addr) <= ntohl(route->nexthop.u.v4.s_addr))))
    {
        g_free(route);
        g_free(key);
        return;
    }
    g_hash_table_replace(desired, key, route);
}

static int ospf_route_equal(const ospf_route_t *a, const ospf_route_t *b)
{
    return a && b && a->prefix_len == b->prefix_len && a->metric == b->metric && a->out_ifindex == b->out_ifindex &&
           net_addr_equal(&a->prefix, &b->prefix) && net_addr_equal(&a->source, &b->source) &&
           net_addr_equal(&a->nexthop, &b->nexthop);
}

static void ospf_spf_reconcile_routes(ospf_instance_t *inst, GHashTable *desired)
{
    GHashTableIter current_iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&current_iter, inst->routes);
    while (g_hash_table_iter_next(&current_iter, &key, &value))
    {
        ospf_route_t *current = (ospf_route_t *)value;
        ospf_route_t *next = g_hash_table_lookup(desired, key);
        if (!next)
        {
            if (ospf_route_sync_enqueue_del(current) != ERRCODE_SUCCESS)
            {
                LOG_WARN("OSPF process %u: failed to queue route withdrawal", inst->process_id);
            }
            continue;
        }
        if (ospf_route_equal(current, next))
        {
            continue;
        }
        if (ospf_route_sync_enqueue_replace(current, next) != ERRCODE_SUCCESS)
        {
            LOG_WARN("OSPF process %u: failed to queue route replacement", inst->process_id);
        }
    }

    GHashTableIter desired_iter;
    g_hash_table_iter_init(&desired_iter, desired);
    while (g_hash_table_iter_next(&desired_iter, &key, &value))
    {
        ospf_route_t *route = (ospf_route_t *)value;
        if (g_hash_table_contains(inst->routes, key))
        {
            continue;
        }
        if (ospf_route_sync_enqueue_add(route) != ERRCODE_SUCCESS)
        {
            LOG_WARN("OSPF process %u: failed to queue route installation", inst->process_id);
        }
    }

    GHashTable *previous = inst->routes;
    inst->routes = desired;
    g_hash_table_destroy(previous);
}

void ospf_spf_recalculate(ospf_instance_t *inst)
{
    if (!inst || !inst->lsdb || !inst->routes || inst->router_id == 0u)
    {
        return;
    }

    uint64_t now_msec = ospf_now_msec();
    GHashTable *desired = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GHashTable *areas = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, NULL);
    if (!desired || !areas)
    {
        if (desired)
        {
            g_hash_table_destroy(desired);
        }
        if (areas)
        {
            g_hash_table_destroy(areas);
        }
        return;
    }

    GHashTableIter lsdb_iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&lsdb_iter, inst->lsdb);
    while (g_hash_table_iter_next(&lsdb_iter, NULL, &value))
    {
        const ospf_lsa_entry_t *entry = (const ospf_lsa_entry_t *)value;
        if (ospf_lsa_current_age(entry, now_msec) >= OSPF_LSA_MAX_AGE)
        {
            continue;
        }
        uint32_t *area_id = g_new(uint32_t, 1);
        if (area_id)
        {
            *area_id = entry->area_id;
            g_hash_table_add(areas, area_id);
        }
    }

    GHashTableIter area_iter;
    gpointer area_key = NULL;
    g_hash_table_iter_init(&area_iter, areas);
    while (g_hash_table_iter_next(&area_iter, &area_key, NULL))
    {
        uint32_t area_id = *(const uint32_t *)area_key;
        ospf_spf_graph_t *graph = ospf_spf_build_graph(inst, area_id, now_msec);
        if (!graph)
        {
            continue;
        }
        ospf_spf_run(graph, inst->router_id);
        for (guint i = 0u; i < graph->prefixes->len; ++i)
        {
            ospf_spf_add_desired_route(inst, graph, g_ptr_array_index(graph->prefixes, i), desired);
        }
        ospf_spf_graph_destroy(graph);
    }

    ospf_spf_reconcile_routes(inst, desired);
    g_hash_table_destroy(areas);
}

static void ospf_spf_recalculate_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    (void)user_data;
    ospf_spf_recalculate((ospf_instance_t *)value);
}

void ospf_spf_recalculate_all(void)
{
    if (g_ospf_work_local && g_ospf_work_local->instances)
    {
        g_hash_table_foreach(g_ospf_work_local->instances, ospf_spf_recalculate_cb, NULL);
    }
}
