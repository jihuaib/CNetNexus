/**
 * @file   ospf_lsa.c
 * @brief  OSPFv2 LSA database, aging, and local origination
 */
#include "ospf_lsa.h"

#include <arpa/inet.h>
#include <string.h>

#include "if.h"
#include "log.h"
#include "ospf_packet.h"
#include "ospf_spf.h"

#define OSPF_OPTIONS_E 0x02u
#define OSPF_LSA_MAX_AGE_DIFF 900u
#define OSPF_ROUTER_LINK_POINT_TO_POINT 1u
#define OSPF_ROUTER_LINK_TRANSIT 2u
#define OSPF_ROUTER_LINK_STUB 3u

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

static void ospf_put_u16(uint8_t *p, uint16_t value)
{
    value = htons(value);
    memcpy(p, &value, sizeof(value));
}

static void ospf_put_u32(uint8_t *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}

static uint32_t ospf_mask_from_prefix(uint8_t prefix_len)
{
    if (prefix_len == 0u)
    {
        return 0u;
    }
    if (prefix_len >= 32u)
    {
        return 0xffffffffu;
    }
    return 0xffffffffu << (32u - prefix_len);
}

static uint32_t ospf_network_address(uint32_t address, uint8_t prefix_len)
{
    return address & ospf_mask_from_prefix(prefix_len);
}

static gboolean ospf_lsa_router_body_valid(const uint8_t *raw, uint16_t length)
{
    if (length < OSPF_LSA_HEADER_LEN + 4u || ospf_get_u32(raw + 4u) != ospf_get_u32(raw + 8u))
    {
        return FALSE;
    }

    const uint8_t *body = raw + OSPF_LSA_HEADER_LEN;
    size_t body_len = (size_t)length - OSPF_LSA_HEADER_LEN;
    uint16_t link_count = ospf_get_u16(body + 2u);
    size_t offset = 4u;
    for (uint16_t i = 0u; i < link_count; ++i)
    {
        if (offset + 12u > body_len)
        {
            return FALSE;
        }
        uint8_t link_type = body[offset + 8u];
        uint8_t tos_count = body[offset + 9u];
        size_t link_len = 12u + ((size_t)tos_count * 4u);
        if (link_type < 1u || link_type > 4u || link_len > body_len - offset)
        {
            return FALSE;
        }
        offset += link_len;
    }
    return offset == body_len;
}

static gboolean ospf_lsa_network_body_valid(const uint8_t *raw, uint16_t length)
{
    if (length < OSPF_LSA_HEADER_LEN + 8u)
    {
        return FALSE;
    }
    size_t body_len = (size_t)length - OSPF_LSA_HEADER_LEN;
    if (((body_len - 4u) % 4u) != 0u)
    {
        return FALSE;
    }
    for (size_t offset = OSPF_LSA_HEADER_LEN + 4u; offset < length; offset += 4u)
    {
        if (ospf_get_u32(raw + offset) == 0u)
        {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean ospf_lsa_structure_valid(const uint8_t *raw, size_t raw_len)
{
    if (!raw || raw_len < OSPF_LSA_HEADER_LEN)
    {
        return FALSE;
    }

    uint16_t length = ospf_get_u16(raw + 18u);
    uint16_t age = ospf_get_u16(raw);
    uint8_t type = raw[3];
    uint32_t advertising_router = ospf_get_u32(raw + 8u);
    uint32_t sequence = ospf_get_u32(raw + 12u);
    if (length != raw_len || age > OSPF_LSA_MAX_AGE || advertising_router == 0u || sequence == 0x80000000u ||
        !ospf_lsa_checksum_valid(raw, length))
    {
        return FALSE;
    }
    if (type == OSPF_LSA_ROUTER)
    {
        return ospf_lsa_router_body_valid(raw, length);
    }
    if (type == OSPF_LSA_NETWORK)
    {
        return ospf_lsa_network_body_valid(raw, length);
    }
    return FALSE;
}

char *ospf_lsa_key_new(uint32_t area_id, uint8_t type, uint32_t link_state_id, uint32_t advertising_router)
{
    return g_strdup_printf("%08x|%u|%08x|%08x", area_id, (unsigned)type, link_state_id, advertising_router);
}

ospf_lsa_entry_t *ospf_lsa_lookup(const ospf_instance_t *inst, uint32_t area_id, uint8_t type, uint32_t link_state_id,
                                  uint32_t advertising_router)
{
    if (!inst || !inst->lsdb)
    {
        return NULL;
    }
    char key[48];
    g_snprintf(key, sizeof(key), "%08x|%u|%08x|%08x", area_id, (unsigned)type, link_state_id, advertising_router);
    return (ospf_lsa_entry_t *)g_hash_table_lookup(inst->lsdb, key);
}

uint16_t ospf_lsa_current_age(const ospf_lsa_entry_t *entry, uint64_t now_msec)
{
    if (!entry)
    {
        return OSPF_LSA_MAX_AGE;
    }
    uint64_t elapsed_sec = 0u;
    if (now_msec >= entry->installed_msec)
    {
        elapsed_sec = (now_msec - entry->installed_msec) / 1000u;
    }
    uint64_t age = (uint64_t)entry->age + elapsed_sec;
    return (age >= OSPF_LSA_MAX_AGE) ? OSPF_LSA_MAX_AGE : (uint16_t)age;
}

int ospf_lsa_compare_header(const uint8_t *header, const ospf_lsa_entry_t *current)
{
    if (!header)
    {
        return -1;
    }
    if (!current)
    {
        return 1;
    }

    int32_t candidate_seq = (int32_t)ospf_get_u32(header + 12u);
    int32_t current_seq = (int32_t)current->sequence;
    if (candidate_seq != current_seq)
    {
        return candidate_seq > current_seq ? 1 : -1;
    }

    uint16_t candidate_checksum = ospf_get_u16(header + 16u);
    if (candidate_checksum != current->checksum)
    {
        return candidate_checksum > current->checksum ? 1 : -1;
    }

    uint16_t candidate_age = ospf_get_u16(header);
    uint16_t current_age = ospf_lsa_current_age(current, ospf_now_msec());
    if (candidate_age == OSPF_LSA_MAX_AGE && current_age != OSPF_LSA_MAX_AGE)
    {
        return 1;
    }
    if (current_age == OSPF_LSA_MAX_AGE && candidate_age != OSPF_LSA_MAX_AGE)
    {
        return -1;
    }
    uint16_t age_diff = (candidate_age > current_age) ? (candidate_age - current_age) : (current_age - candidate_age);
    if (age_diff > OSPF_LSA_MAX_AGE_DIFF)
    {
        return candidate_age < current_age ? 1 : -1;
    }
    return 0;
}

int ospf_lsa_install(ospf_instance_t *inst, uint32_t area_id, const uint8_t *raw, size_t raw_len, uint64_t now_msec,
                     int self_originated, int *changed_out, int *comparison_out)
{
    if (changed_out)
    {
        *changed_out = 0;
    }
    if (comparison_out)
    {
        *comparison_out = 0;
    }
    if (!inst || !ospf_lsa_structure_valid(raw, raw_len))
    {
        return -1;
    }

    uint16_t length = ospf_get_u16(raw + 18u);
    uint16_t age = ospf_get_u16(raw);
    uint8_t type = raw[3];
    uint32_t link_state_id = ospf_get_u32(raw + 4u);
    uint32_t advertising_router = ospf_get_u32(raw + 8u);
    ospf_lsa_entry_t *current = ospf_lsa_lookup(inst, area_id, type, link_state_id, advertising_router);
    int comparison = ospf_lsa_compare_header(raw, current);
    if (comparison_out)
    {
        *comparison_out = comparison;
    }
    if (comparison < 0)
    {
        return 0;
    }
    if (comparison == 0 && current)
    {
        return 0;
    }

    char *key = ospf_lsa_key_new(area_id, type, link_state_id, advertising_router);
    if (!key)
    {
        return -1;
    }
    ospf_lsa_entry_t *entry = g_malloc0(sizeof(*entry));
    if (!entry)
    {
        g_free(key);
        return -1;
    }
    entry->raw = g_byte_array_sized_new(length);
    if (!entry->raw)
    {
        g_free(entry);
        g_free(key);
        return -1;
    }
    g_byte_array_append(entry->raw, raw, length);
    entry->area_id = area_id;
    entry->age = age;
    entry->options = raw[2];
    entry->type = type;
    entry->link_state_id = link_state_id;
    entry->advertising_router = advertising_router;
    entry->sequence = ospf_get_u32(raw + 12u);
    entry->checksum = ospf_get_u16(raw + 16u);
    entry->length = length;
    entry->installed_msec = now_msec;
    entry->self_originated = self_originated ? 1u : 0u;
    g_hash_table_replace(inst->lsdb, key, entry);
    if (changed_out)
    {
        *changed_out = 1;
    }
    return 0;
}

static gboolean ospf_if_is_ready(const ospf_if_cfg_t *cfg, const if_api_cache_entry_t **if_entry_out)
{
    const if_api_cache_entry_t *if_entry = cfg ? if_api_cache_lookup(cfg->ifname) : NULL;
    if (if_entry_out)
    {
        *if_entry_out = if_entry;
    }
    return cfg && cfg->enabled && ospf_if_entry_matches_vrf(cfg->vrf_name, if_entry) && if_entry->link_up &&
           if_entry->proto_up && if_entry->ifindex != 0u && if_entry->ipv4_addr.family == AF_INET;
}

static gboolean ospf_neighbor_is_full_on_if(const ospf_neighbor_t *nbr, const ospf_if_cfg_t *cfg)
{
    return nbr && cfg && nbr->state == OSPF_NBR_STATE_FULL && nbr->area_id == cfg->area_id &&
           strcmp(nbr->ifname, cfg->ifname) == 0;
}

static uint32_t ospf_count_full_neighbors(const ospf_instance_t *inst, const ospf_if_cfg_t *cfg)
{
    uint32_t count = 0u;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        if (ospf_neighbor_is_full_on_if((const ospf_neighbor_t *)value, cfg))
        {
            count++;
        }
    }
    return count;
}

static void ospf_append_router_link(GByteArray *body, uint32_t link_id, uint32_t link_data, uint8_t link_type,
                                    uint16_t metric)
{
    uint8_t link[12] = {0};
    ospf_put_u32(link, link_id);
    ospf_put_u32(link + 4u, link_data);
    link[8] = link_type;
    link[9] = 0u;
    ospf_put_u16(link + 10u, metric);
    g_byte_array_append(body, link, sizeof(link));
}

static void ospf_append_stub_link(GByteArray *body, const if_api_cache_entry_t *if_entry, const ospf_if_cfg_t *cfg)
{
    uint32_t address = ntohl(if_entry->ipv4_addr.u.v4.s_addr);
    uint8_t prefix_len = if_entry->ipv4_prefix_len;
    if (g_str_has_prefix(cfg->ifname, "loop"))
    {
        prefix_len = 32u;
    }
    uint32_t mask = ospf_mask_from_prefix(prefix_len);
    ospf_append_router_link(body, ospf_network_address(address, prefix_len), mask, OSPF_ROUTER_LINK_STUB, cfg->cost);
}

static void ospf_append_p2p_links(GByteArray *body, const ospf_instance_t *inst, const ospf_if_cfg_t *cfg,
                                  uint32_t local_address)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_neighbor_t *nbr = (const ospf_neighbor_t *)value;
        if (ospf_neighbor_is_full_on_if(nbr, cfg))
        {
            ospf_append_router_link(body, nbr->router_id, local_address, OSPF_ROUTER_LINK_POINT_TO_POINT, cfg->cost);
        }
    }
}

static uint32_t ospf_lsa_next_sequence(const ospf_lsa_entry_t *current)
{
    if (!current)
    {
        return OSPF_LSA_INITIAL_SEQUENCE;
    }
    return current->sequence + 1u;
}

static GByteArray *ospf_build_lsa(uint8_t type, uint32_t link_state_id, uint32_t advertising_router, uint32_t sequence,
                                  const uint8_t *body, size_t body_len)
{
    size_t total_len = OSPF_LSA_HEADER_LEN + body_len;
    if (total_len > UINT16_MAX)
    {
        return NULL;
    }
    GByteArray *raw = g_byte_array_sized_new(total_len);
    if (!raw)
    {
        return NULL;
    }
    g_byte_array_set_size(raw, total_len);
    memset(raw->data, 0, total_len);
    raw->data[2] = OSPF_OPTIONS_E;
    raw->data[3] = type;
    ospf_put_u32(raw->data + 4u, link_state_id);
    ospf_put_u32(raw->data + 8u, advertising_router);
    ospf_put_u32(raw->data + 12u, sequence);
    ospf_put_u16(raw->data + 18u, (uint16_t)total_len);
    if (body_len > 0u)
    {
        memcpy(raw->data + OSPF_LSA_HEADER_LEN, body, body_len);
    }
    ospf_put_u16(raw->data + 16u, ospf_lsa_checksum(raw->data, raw->len));
    return raw;
}

static gboolean ospf_lsa_body_matches(const ospf_lsa_entry_t *current, const GByteArray *candidate)
{
    if (!current || !current->raw || !candidate || current->raw->len != candidate->len ||
        candidate->len < OSPF_LSA_HEADER_LEN)
    {
        return FALSE;
    }
    return memcmp(current->raw->data + OSPF_LSA_HEADER_LEN, candidate->data + OSPF_LSA_HEADER_LEN,
                  candidate->len - OSPF_LSA_HEADER_LEN) == 0;
}

static void ospf_lsa_mark_maxage(ospf_instance_t *inst, ospf_lsa_entry_t *entry, uint64_t now_msec)
{
    if (!inst || !entry || !entry->raw || entry->raw->len < OSPF_LSA_HEADER_LEN || entry->age == OSPF_LSA_MAX_AGE)
    {
        return;
    }

    ospf_put_u16(entry->raw->data, OSPF_LSA_MAX_AGE);
    entry->age = OSPF_LSA_MAX_AGE;
    entry->installed_msec = now_msec;
    ospf_packet_flood_lsa(inst, entry, NULL);
}

static void ospf_install_and_flood_self(ospf_instance_t *inst, uint32_t area_id, GByteArray *raw,
                                        GHashTable *desired_keys, uint64_t now_msec)
{
    uint8_t type = raw->data[3];
    uint32_t link_state_id = ospf_get_u32(raw->data + 4u);
    uint32_t advertising_router = ospf_get_u32(raw->data + 8u);
    char *desired_key = ospf_lsa_key_new(area_id, type, link_state_id, advertising_router);
    if (desired_key)
    {
        g_hash_table_add(desired_keys, desired_key);
    }

    ospf_lsa_entry_t *current = ospf_lsa_lookup(inst, area_id, type, link_state_id, advertising_router);
    if (current && current->self_originated && ospf_lsa_body_matches(current, raw) &&
        ospf_lsa_current_age(current, now_msec) < OSPF_LSA_REFRESH_TIME)
    {
        g_byte_array_unref(raw);
        return;
    }

    if (current)
    {
        if (current->sequence == 0x7fffffffu)
        {
            ospf_lsa_mark_maxage(inst, current, now_msec);
            uint64_t retry_msec = now_msec + 1000u;
            if (inst->next_lsa_originate_msec == 0u || retry_msec < inst->next_lsa_originate_msec)
            {
                inst->next_lsa_originate_msec = retry_msec;
            }
            g_byte_array_unref(raw);
            return;
        }

        uint64_t earliest_msec = current->installed_msec + ((uint64_t)OSPF_LSA_MIN_INTERVAL * 1000u);
        if (now_msec < earliest_msec)
        {
            if (inst->next_lsa_originate_msec == 0u || earliest_msec < inst->next_lsa_originate_msec)
            {
                inst->next_lsa_originate_msec = earliest_msec;
            }
            g_byte_array_unref(raw);
            return;
        }
    }

    uint32_t sequence = ospf_lsa_next_sequence(current);
    ospf_put_u32(raw->data + 12u, sequence);
    ospf_put_u16(raw->data + 16u, 0u);
    ospf_put_u16(raw->data + 16u, ospf_lsa_checksum(raw->data, raw->len));

    int changed = 0;
    if (ospf_lsa_install(inst, area_id, raw->data, raw->len, now_msec, 1, &changed, NULL) == 0 && changed)
    {
        ospf_lsa_entry_t *installed = ospf_lsa_lookup(inst, area_id, type, link_state_id, advertising_router);
        if (installed)
        {
            ospf_packet_flood_lsa(inst, installed, NULL);
        }
    }
    g_byte_array_unref(raw);
}

static void ospf_originate_router_lsa(ospf_instance_t *inst, uint32_t area_id, GHashTable *desired_keys,
                                      uint64_t now_msec)
{
    GByteArray *body = g_byte_array_new();
    if (!body)
    {
        return;
    }
    uint8_t router_header[4] = {0};
    g_byte_array_append(body, router_header, sizeof(router_header));
    uint16_t link_count = 0u;

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_if_cfg_t *cfg = (const ospf_if_cfg_t *)value;
        const if_api_cache_entry_t *if_entry = NULL;
        if (cfg->area_id != area_id || !ospf_if_is_ready(cfg, &if_entry))
        {
            continue;
        }

        uint32_t local_address = ntohl(if_entry->ipv4_addr.u.v4.s_addr);
        uint32_t full_count = ospf_count_full_neighbors(inst, cfg);
        if (cfg->passive || g_str_has_prefix(cfg->ifname, "loop"))
        {
            ospf_append_stub_link(body, if_entry, cfg);
            link_count++;
        }
        else if (cfg->network_type == OSPF_NETWORK_POINT_TO_POINT)
        {
            ospf_append_p2p_links(body, inst, cfg, local_address);
            link_count = (uint16_t)(link_count + full_count);
            ospf_append_stub_link(body, if_entry, cfg);
            link_count++;
        }
        else if (full_count > 0u && cfg->dr != 0u)
        {
            ospf_append_router_link(body, cfg->dr, local_address, OSPF_ROUTER_LINK_TRANSIT, cfg->cost);
            link_count++;
        }
        else
        {
            ospf_append_stub_link(body, if_entry, cfg);
            link_count++;
        }
    }

    ospf_put_u16(body->data + 2u, link_count);
    GByteArray *raw = ospf_build_lsa(OSPF_LSA_ROUTER, inst->router_id, inst->router_id, OSPF_LSA_INITIAL_SEQUENCE,
                                     body->data, body->len);
    g_byte_array_unref(body);
    if (raw)
    {
        ospf_install_and_flood_self(inst, area_id, raw, desired_keys, now_msec);
    }
}

static void ospf_originate_network_lsa(ospf_instance_t *inst, const ospf_if_cfg_t *cfg,
                                       const if_api_cache_entry_t *if_entry, GHashTable *desired_keys,
                                       uint64_t now_msec)
{
    uint32_t full_count = ospf_count_full_neighbors(inst, cfg);
    if (cfg->network_type != OSPF_NETWORK_BROADCAST || cfg->state != OSPF_IF_STATE_DR || full_count == 0u)
    {
        return;
    }

    GByteArray *body = g_byte_array_new();
    if (!body)
    {
        return;
    }
    uint8_t value[4];
    ospf_put_u32(value, ospf_mask_from_prefix(if_entry->ipv4_prefix_len));
    g_byte_array_append(body, value, sizeof(value));
    ospf_put_u32(value, inst->router_id);
    g_byte_array_append(body, value, sizeof(value));

    GHashTableIter iter;
    gpointer nbr_value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &nbr_value))
    {
        const ospf_neighbor_t *nbr = (const ospf_neighbor_t *)nbr_value;
        if (ospf_neighbor_is_full_on_if(nbr, cfg))
        {
            ospf_put_u32(value, nbr->router_id);
            g_byte_array_append(body, value, sizeof(value));
        }
    }

    uint32_t local_address = ntohl(if_entry->ipv4_addr.u.v4.s_addr);
    GByteArray *raw = ospf_build_lsa(OSPF_LSA_NETWORK, local_address, inst->router_id, OSPF_LSA_INITIAL_SEQUENCE,
                                     body->data, body->len);
    g_byte_array_unref(body);
    if (raw)
    {
        ospf_install_and_flood_self(inst, cfg->area_id, raw, desired_keys, now_msec);
    }
}

typedef struct ospf_area_collect
{
    GHashTable *areas;
} ospf_area_collect_t;

static void ospf_collect_area(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const ospf_if_cfg_t *cfg = (const ospf_if_cfg_t *)value;
    ospf_area_collect_t *ctx = (ospf_area_collect_t *)user_data;
    if (cfg && ospf_if_is_ready(cfg, NULL))
    {
        uint32_t *area_id = g_new(uint32_t, 1);
        if (area_id)
        {
            *area_id = cfg->area_id;
            g_hash_table_add(ctx->areas, area_id);
        }
    }
}

static void ospf_flush_stale_self_lsas(ospf_instance_t *inst, GHashTable *desired_keys)
{
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        ospf_lsa_entry_t *entry = (ospf_lsa_entry_t *)value;
        if (entry->advertising_router != inst->router_id || g_hash_table_contains(desired_keys, key))
        {
            continue;
        }
        ospf_lsa_mark_maxage(inst, entry, ospf_now_msec());
    }
}

static gboolean ospf_lsa_flush_complete(const ospf_instance_t *inst, const ospf_lsa_entry_t *entry)
{
    char key[48];
    g_snprintf(key, sizeof(key), "%08x|%u|%08x|%08x", entry->area_id, (unsigned)entry->type, entry->link_state_id,
               entry->advertising_router);

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_neighbor_t *nbr = (const ospf_neighbor_t *)value;
        if (nbr->area_id != entry->area_id)
        {
            continue;
        }
        if ((nbr->state >= OSPF_NBR_STATE_EXSTART && nbr->state <= OSPF_NBR_STATE_LOADING) ||
            (nbr->retrans_lsas && g_hash_table_contains(nbr->retrans_lsas, key)))
        {
            return FALSE;
        }
    }
    return TRUE;
}

void ospf_lsa_originate_all(ospf_instance_t *inst, uint64_t now_msec)
{
    if (!inst || !inst->admin_up || inst->router_id == 0u)
    {
        return;
    }

    GHashTable *areas = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, NULL);
    GHashTable *desired_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!areas || !desired_keys)
    {
        if (areas)
        {
            g_hash_table_destroy(areas);
        }
        if (desired_keys)
        {
            g_hash_table_destroy(desired_keys);
        }
        return;
    }

    ospf_area_collect_t collect = {.areas = areas};
    g_hash_table_foreach(inst->if_cfgs, ospf_collect_area, &collect);

    GHashTableIter area_iter;
    gpointer area_key = NULL;
    g_hash_table_iter_init(&area_iter, areas);
    while (g_hash_table_iter_next(&area_iter, &area_key, NULL))
    {
        uint32_t area_id = *(const uint32_t *)area_key;
        ospf_originate_router_lsa(inst, area_id, desired_keys, now_msec);
    }

    GHashTableIter if_iter;
    gpointer if_value = NULL;
    g_hash_table_iter_init(&if_iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&if_iter, NULL, &if_value))
    {
        const ospf_if_cfg_t *cfg = (const ospf_if_cfg_t *)if_value;
        const if_api_cache_entry_t *if_entry = NULL;
        if (ospf_if_is_ready(cfg, &if_entry))
        {
            ospf_originate_network_lsa(inst, cfg, if_entry, desired_keys, now_msec);
        }
    }

    ospf_flush_stale_self_lsas(inst, desired_keys);
    inst->last_lsa_refresh_msec = now_msec;
    g_hash_table_destroy(desired_keys);
    g_hash_table_destroy(areas);
}

void ospf_lsa_age(ospf_instance_t *inst, uint64_t now_msec)
{
    if (!inst || !inst->lsdb)
    {
        return;
    }

    gboolean changed = FALSE;
    gboolean refresh = FALSE;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        ospf_lsa_entry_t *entry = (ospf_lsa_entry_t *)value;
        uint16_t age = ospf_lsa_current_age(entry, now_msec);
        if (age >= OSPF_LSA_MAX_AGE)
        {
            if (entry->age != OSPF_LSA_MAX_AGE)
            {
                ospf_lsa_mark_maxage(inst, entry, now_msec);
                changed = TRUE;
            }
            if (ospf_lsa_flush_complete(inst, entry))
            {
                gboolean was_self_originated = entry->self_originated != 0u;
                g_hash_table_iter_remove(&iter);
                changed = TRUE;
                refresh = refresh || was_self_originated;
            }
        }
        else if (entry->self_originated && age >= OSPF_LSA_REFRESH_TIME)
        {
            refresh = TRUE;
        }
    }

    gboolean originate_pending = inst->next_lsa_originate_msec != 0u && now_msec >= inst->next_lsa_originate_msec;
    if (originate_pending)
    {
        inst->next_lsa_originate_msec = 0u;
    }
    if (refresh || originate_pending)
    {
        ospf_lsa_originate_all(inst, now_msec);
    }
    if (changed || refresh || originate_pending)
    {
        ospf_spf_recalculate(inst);
    }
}

void ospf_lsa_flush_self(ospf_instance_t *inst)
{
    if (!inst || !inst->lsdb)
    {
        return;
    }
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        ospf_lsa_entry_t *entry = (ospf_lsa_entry_t *)value;
        if (!entry->self_originated)
        {
            continue;
        }
        if (entry->raw)
        {
            ospf_put_u16(entry->raw->data, OSPF_LSA_MAX_AGE);
            entry->age = OSPF_LSA_MAX_AGE;
            ospf_packet_flood_lsa(inst, entry, NULL);
        }
        g_hash_table_iter_remove(&iter);
    }
}
