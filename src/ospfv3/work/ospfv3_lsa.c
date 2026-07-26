/**
 * @file   ospfv3_lsa.c
 * @brief  OSPFv3 LSA validation, database maintenance, and origination
 */
#include "ospfv3_lsa.h"

#include <arpa/inet.h>
#include <string.h>

#include "if.h"
#include "ospfv3_packet.h"
#include "ospfv3_spf.h"

#define OSPFV3_LSA_MAX_AGE_DIFF 900u
#define OSPFV3_ROUTER_LINK_POINT_TO_POINT 1u
#define OSPFV3_ROUTER_LINK_TRANSIT 2u
#define OSPFV3_PREFIX_OPTION_LA 0x02u

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

static void ospfv3_put_u16(uint8_t *p, uint16_t value)
{
    value = htons(value);
    memcpy(p, &value, sizeof(value));
}

static void ospfv3_put_u32(uint8_t *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}

static void ospfv3_put_u24(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 16u);
    p[1] = (uint8_t)(value >> 8u);
    p[2] = (uint8_t)value;
}

static size_t ospfv3_prefix_wire_bytes(uint8_t prefix_len)
{
    return ((size_t)prefix_len + 31u) / 32u * 4u;
}

static gboolean ospfv3_prefix_list_valid(const uint8_t *data, size_t len, uint32_t count)
{
    size_t offset = 0u;
    for (uint32_t i = 0u; i < count; ++i)
    {
        if (len - offset < 4u || data[offset] > 128u)
        {
            return FALSE;
        }
        size_t item_len = 4u + ospfv3_prefix_wire_bytes(data[offset]);
        if (item_len > len - offset)
        {
            return FALSE;
        }
        offset += item_len;
    }
    return offset == len;
}

static gboolean ospfv3_lsa_structure_valid(const uint8_t *raw, size_t raw_len)
{
    if (!raw || raw_len < OSPFV3_LSA_HEADER_LEN)
    {
        return FALSE;
    }
    uint16_t length = ospfv3_get_u16(raw + 18u);
    uint16_t type = ospfv3_get_u16(raw + 2u);
    if (length != raw_len || ospfv3_get_u16(raw) > OSPFV3_LSA_MAX_AGE ||
        (int32_t)ospfv3_get_u32(raw + 12u) == INT32_MIN || !ospfv3_lsa_checksum_valid(raw, length))
    {
        return FALSE;
    }

    size_t body_len = length - OSPFV3_LSA_HEADER_LEN;
    const uint8_t *body = raw + OSPFV3_LSA_HEADER_LEN;
    switch (type)
    {
        case OSPFV3_LSA_ROUTER:
            return body_len >= 4u && ((body_len - 4u) % 16u) == 0u;
        case OSPFV3_LSA_NETWORK:
            return body_len >= 8u && ((body_len - 4u) % 4u) == 0u;
        case OSPFV3_LSA_LINK:
            return body_len >= 24u && ospfv3_prefix_list_valid(body + 24u, body_len - 24u, ospfv3_get_u32(body + 20u));
        case OSPFV3_LSA_INTRA_AREA_PREFIX:
            return body_len >= 12u && ospfv3_prefix_list_valid(body + 12u, body_len - 12u, ospfv3_get_u16(body));
        default:
            return FALSE;
    }
}

char *ospfv3_lsa_key_new(uint32_t area_id, uint16_t type, uint32_t link_state_id, uint32_t advertising_router)
{
    return g_strdup_printf("%08x|%04x|%08x|%08x", area_id, type, link_state_id, advertising_router);
}

ospfv3_lsa_entry_t *ospfv3_lsa_lookup(const ospfv3_instance_t *inst, uint32_t area_id, uint16_t type,
                                      uint32_t link_state_id, uint32_t advertising_router)
{
    if (!inst || !inst->lsdb)
    {
        return NULL;
    }
    char key[48];
    g_snprintf(key, sizeof(key), "%08x|%04x|%08x|%08x", area_id, type, link_state_id, advertising_router);
    return g_hash_table_lookup(inst->lsdb, key);
}

uint16_t ospfv3_lsa_current_age(const ospfv3_lsa_entry_t *entry, uint64_t now_msec)
{
    if (!entry)
    {
        return OSPFV3_LSA_MAX_AGE;
    }
    uint64_t elapsed = now_msec >= entry->installed_msec ? (now_msec - entry->installed_msec) / 1000u : 0u;
    uint64_t age = (uint64_t)entry->age + elapsed;
    return age >= OSPFV3_LSA_MAX_AGE ? OSPFV3_LSA_MAX_AGE : (uint16_t)age;
}

int ospfv3_lsa_compare_header(const uint8_t *header, const ospfv3_lsa_entry_t *current)
{
    if (!header)
    {
        return -1;
    }
    if (!current)
    {
        return 1;
    }
    int32_t candidate_seq = (int32_t)ospfv3_get_u32(header + 12u);
    int32_t current_seq = (int32_t)current->sequence;
    if (candidate_seq != current_seq)
    {
        return candidate_seq > current_seq ? 1 : -1;
    }
    uint16_t candidate_checksum = ospfv3_get_u16(header + 16u);
    if (candidate_checksum != current->checksum)
    {
        return candidate_checksum > current->checksum ? 1 : -1;
    }
    uint16_t candidate_age = ospfv3_get_u16(header);
    uint16_t current_age = ospfv3_lsa_current_age(current, ospfv3_now_msec());
    if (candidate_age == OSPFV3_LSA_MAX_AGE && current_age != OSPFV3_LSA_MAX_AGE)
    {
        return 1;
    }
    if (current_age == OSPFV3_LSA_MAX_AGE && candidate_age != OSPFV3_LSA_MAX_AGE)
    {
        return -1;
    }
    uint16_t diff = candidate_age > current_age ? candidate_age - current_age : current_age - candidate_age;
    return diff > OSPFV3_LSA_MAX_AGE_DIFF ? (candidate_age < current_age ? 1 : -1) : 0;
}

int ospfv3_lsa_install(ospfv3_instance_t *inst, uint32_t area_id, const uint8_t *raw, size_t raw_len, uint64_t now_msec,
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
    if (!inst || !ospfv3_lsa_structure_valid(raw, raw_len))
    {
        return -1;
    }

    uint16_t type = ospfv3_get_u16(raw + 2u);
    uint32_t link_state_id = ospfv3_get_u32(raw + 4u);
    uint32_t advertising_router = ospfv3_get_u32(raw + 8u);
    ospfv3_lsa_entry_t *current = ospfv3_lsa_lookup(inst, area_id, type, link_state_id, advertising_router);
    int comparison = ospfv3_lsa_compare_header(raw, current);
    if (comparison_out)
    {
        *comparison_out = comparison;
    }
    if (comparison <= 0)
    {
        return 0;
    }

    char *key = ospfv3_lsa_key_new(area_id, type, link_state_id, advertising_router);
    ospfv3_lsa_entry_t *entry = g_malloc0(sizeof(*entry));
    if (!key || !entry)
    {
        g_free(key);
        g_free(entry);
        return -1;
    }
    entry->raw = g_byte_array_sized_new(raw_len);
    if (!entry->raw)
    {
        g_free(key);
        g_free(entry);
        return -1;
    }
    g_byte_array_append(entry->raw, raw, raw_len);
    entry->area_id = area_id;
    entry->age = ospfv3_get_u16(raw);
    entry->type = type;
    entry->link_state_id = link_state_id;
    entry->advertising_router = advertising_router;
    entry->sequence = ospfv3_get_u32(raw + 12u);
    entry->checksum = ospfv3_get_u16(raw + 16u);
    entry->length = (uint16_t)raw_len;
    entry->installed_msec = now_msec;
    entry->self_originated = self_originated ? 1u : 0u;
    if (type == OSPFV3_LSA_ROUTER || type == OSPFV3_LSA_NETWORK || type == OSPFV3_LSA_LINK)
    {
        const uint8_t *body = raw + OSPFV3_LSA_HEADER_LEN;
        entry->options = ((uint32_t)body[1] << 16u) | ((uint32_t)body[2] << 8u) | body[3];
    }
    g_hash_table_replace(inst->lsdb, key, entry);
    if (changed_out)
    {
        *changed_out = 1;
    }
    return 0;
}

static gboolean ospfv3_if_is_ready(const ospfv3_if_cfg_t *cfg, const if_api_cache_entry_t **entry_out)
{
    const if_api_cache_entry_t *entry = cfg ? if_api_cache_lookup(cfg->ifname) : NULL;
    if (entry_out)
    {
        *entry_out = entry;
    }
    if (!cfg || !cfg->enabled || !ospfv3_if_entry_matches_vrf(cfg->vrf_name, entry) || !entry->link_up ||
        entry->ifindex == 0u)
    {
        return FALSE;
    }
    return cfg->passive ? entry->ipv6_addr.family == AF_INET6 : entry->ipv6_linklocal_addr.family == AF_INET6;
}

static gboolean ospfv3_neighbor_is_full_on_if(const ospfv3_neighbor_t *nbr, const ospfv3_if_cfg_t *cfg)
{
    return nbr && cfg && nbr->state == OSPFV3_NBR_STATE_FULL && nbr->area_id == cfg->area_id &&
           strcmp(nbr->ifname, cfg->ifname) == 0;
}

static uint32_t ospfv3_count_full_neighbors(const ospfv3_instance_t *inst, const ospfv3_if_cfg_t *cfg)
{
    uint32_t count = 0u;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        count += ospfv3_neighbor_is_full_on_if(value, cfg) ? 1u : 0u;
    }
    return count;
}

static GByteArray *ospfv3_build_lsa(uint16_t type, uint32_t link_state_id, uint32_t advertising_router,
                                    const uint8_t *body, size_t body_len)
{
    size_t total_len = OSPFV3_LSA_HEADER_LEN + body_len;
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
    ospfv3_put_u16(raw->data + 2u, type);
    ospfv3_put_u32(raw->data + 4u, link_state_id);
    ospfv3_put_u32(raw->data + 8u, advertising_router);
    ospfv3_put_u32(raw->data + 12u, OSPFV3_LSA_INITIAL_SEQUENCE);
    ospfv3_put_u16(raw->data + 18u, (uint16_t)total_len);
    if (body_len)
    {
        memcpy(raw->data + OSPFV3_LSA_HEADER_LEN, body, body_len);
    }
    ospfv3_put_u16(raw->data + 16u, ospfv3_lsa_checksum(raw->data, raw->len));
    return raw;
}

static gboolean ospfv3_lsa_body_matches(const ospfv3_lsa_entry_t *current, const GByteArray *candidate)
{
    return current && current->raw && candidate && current->raw->len == candidate->len &&
           candidate->len >= OSPFV3_LSA_HEADER_LEN &&
           memcmp(current->raw->data + OSPFV3_LSA_HEADER_LEN, candidate->data + OSPFV3_LSA_HEADER_LEN,
                  candidate->len - OSPFV3_LSA_HEADER_LEN) == 0;
}

static void ospfv3_lsa_mark_maxage(ospfv3_instance_t *inst, ospfv3_lsa_entry_t *entry, uint64_t now_msec)
{
    if (!inst || !entry || !entry->raw || entry->age == OSPFV3_LSA_MAX_AGE)
    {
        return;
    }
    ospfv3_put_u16(entry->raw->data, OSPFV3_LSA_MAX_AGE);
    entry->age = OSPFV3_LSA_MAX_AGE;
    entry->installed_msec = now_msec;
    ospfv3_packet_flood_lsa(inst, entry, NULL);
}

static void ospfv3_install_and_flood_self(ospfv3_instance_t *inst, uint32_t area_id, GByteArray *raw,
                                          GHashTable *desired_keys, uint64_t now_msec)
{
    uint16_t type = ospfv3_get_u16(raw->data + 2u);
    uint32_t lsid = ospfv3_get_u32(raw->data + 4u);
    uint32_t adv = ospfv3_get_u32(raw->data + 8u);
    char *desired_key = ospfv3_lsa_key_new(area_id, type, lsid, adv);
    if (desired_key)
    {
        g_hash_table_add(desired_keys, desired_key);
    }

    ospfv3_lsa_entry_t *current = ospfv3_lsa_lookup(inst, area_id, type, lsid, adv);
    if (current && current->self_originated && ospfv3_lsa_body_matches(current, raw) &&
        ospfv3_lsa_current_age(current, now_msec) < OSPFV3_LSA_REFRESH_TIME)
    {
        g_byte_array_unref(raw);
        return;
    }
    if (current)
    {
        if (current->sequence == 0x7fffffffu)
        {
            ospfv3_lsa_mark_maxage(inst, current, now_msec);
            inst->next_lsa_originate_msec = now_msec + 1000u;
            g_byte_array_unref(raw);
            return;
        }
        uint64_t earliest = current->installed_msec + (uint64_t)OSPFV3_LSA_MIN_INTERVAL * 1000u;
        if (now_msec < earliest)
        {
            if (inst->next_lsa_originate_msec == 0u || earliest < inst->next_lsa_originate_msec)
            {
                inst->next_lsa_originate_msec = earliest;
            }
            g_byte_array_unref(raw);
            return;
        }
        ospfv3_put_u32(raw->data + 12u, current->sequence + 1u);
        ospfv3_put_u16(raw->data + 16u, 0u);
        ospfv3_put_u16(raw->data + 16u, ospfv3_lsa_checksum(raw->data, raw->len));
    }

    int changed = 0;
    if (ospfv3_lsa_install(inst, area_id, raw->data, raw->len, now_msec, 1, &changed, NULL) == 0 && changed)
    {
        ospfv3_lsa_entry_t *installed = ospfv3_lsa_lookup(inst, area_id, type, lsid, adv);
        if (installed)
        {
            ospfv3_packet_flood_lsa(inst, installed, NULL);
        }
    }
    g_byte_array_unref(raw);
}

static void ospfv3_append_router_link(GByteArray *body, uint8_t type, uint16_t metric, uint32_t interface_id,
                                      uint32_t neighbor_interface_id, uint32_t neighbor_router_id)
{
    uint8_t link[16] = {0};
    link[0] = type;
    ospfv3_put_u16(link + 2u, metric);
    ospfv3_put_u32(link + 4u, interface_id);
    ospfv3_put_u32(link + 8u, neighbor_interface_id);
    ospfv3_put_u32(link + 12u, neighbor_router_id);
    g_byte_array_append(body, link, sizeof(link));
}

static void ospfv3_originate_router_lsa(ospfv3_instance_t *inst, uint32_t area_id, GHashTable *desired,
                                        uint64_t now_msec)
{
    GByteArray *body = g_byte_array_new();
    uint8_t header[4] = {0};
    ospfv3_put_u24(header + 1u, OSPFV3_OPTIONS_DEFAULT);
    g_byte_array_append(body, header, sizeof(header));

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_if_cfg_t *cfg = value;
        const if_api_cache_entry_t *entry = NULL;
        if (cfg->area_id != area_id || cfg->passive || !ospfv3_if_is_ready(cfg, &entry))
        {
            continue;
        }
        if (cfg->network_type == OSPFV3_NETWORK_POINT_TO_POINT)
        {
            GHashTableIter nbr_iter;
            gpointer nbr_value = NULL;
            g_hash_table_iter_init(&nbr_iter, inst->neighbors);
            while (g_hash_table_iter_next(&nbr_iter, NULL, &nbr_value))
            {
                const ospfv3_neighbor_t *nbr = nbr_value;
                if (ospfv3_neighbor_is_full_on_if(nbr, cfg))
                {
                    ospfv3_append_router_link(body, OSPFV3_ROUTER_LINK_POINT_TO_POINT, cfg->cost, entry->ifindex,
                                              nbr->interface_id, nbr->router_id);
                }
            }
        }
        else if (ospfv3_count_full_neighbors(inst, cfg) > 0u && cfg->dr != 0u)
        {
            uint32_t dr_interface_id = entry->ifindex;
            GHashTableIter nbr_iter;
            gpointer nbr_value = NULL;
            g_hash_table_iter_init(&nbr_iter, inst->neighbors);
            while (g_hash_table_iter_next(&nbr_iter, NULL, &nbr_value))
            {
                const ospfv3_neighbor_t *nbr = nbr_value;
                if (ospfv3_neighbor_is_full_on_if(nbr, cfg) && nbr->router_id == cfg->dr)
                {
                    dr_interface_id = nbr->interface_id;
                    break;
                }
            }
            ospfv3_append_router_link(body, OSPFV3_ROUTER_LINK_TRANSIT, cfg->cost, entry->ifindex, dr_interface_id,
                                      cfg->dr);
        }
    }
    GByteArray *raw = ospfv3_build_lsa(OSPFV3_LSA_ROUTER, 0u, inst->router_id, body->data, body->len);
    g_byte_array_unref(body);
    if (raw)
    {
        ospfv3_install_and_flood_self(inst, area_id, raw, desired, now_msec);
    }
}

static void ospfv3_append_prefix(GByteArray *body, const net_addr_t *address, uint8_t prefix_len, uint8_t options,
                                 uint16_t metric)
{
    net_addr_t normalized = *address;
    if (normalized.family != AF_INET6 || net_addr_prefix_normalize(&normalized, prefix_len) != 0)
    {
        return;
    }
    uint8_t header[4] = {prefix_len, options, 0u, 0u};
    ospfv3_put_u16(header + 2u, metric);
    g_byte_array_append(body, header, sizeof(header));
    g_byte_array_append(body, normalized.u.v6.s6_addr, ospfv3_prefix_wire_bytes(prefix_len));
}

static void ospfv3_originate_intra_prefix_lsa(ospfv3_instance_t *inst, uint32_t area_id, GHashTable *desired,
                                              uint64_t now_msec)
{
    GByteArray *body = g_byte_array_sized_new(64u);
    uint8_t header[12] = {0};
    ospfv3_put_u16(header + 2u, OSPFV3_LSA_ROUTER);
    ospfv3_put_u32(header + 8u, inst->router_id);
    g_byte_array_append(body, header, sizeof(header));
    uint16_t count = 0u;

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_if_cfg_t *cfg = value;
        const if_api_cache_entry_t *entry = NULL;
        if (cfg->area_id != area_id || !ospfv3_if_is_ready(cfg, &entry) || entry->ipv6_addr.family != AF_INET6 ||
            IN6_IS_ADDR_LINKLOCAL(&entry->ipv6_addr.u.v6))
        {
            continue;
        }
        uint8_t prefix_len = g_str_has_prefix(cfg->ifname, "loop") ? 128u : entry->ipv6_prefix_len;
        uint8_t options = prefix_len == 128u ? OSPFV3_PREFIX_OPTION_LA : 0u;
        ospfv3_append_prefix(body, &entry->ipv6_addr, prefix_len, options, cfg->cost);
        count++;
    }
    ospfv3_put_u16(body->data, count);
    GByteArray *raw = ospfv3_build_lsa(OSPFV3_LSA_INTRA_AREA_PREFIX, 0u, inst->router_id, body->data, body->len);
    g_byte_array_unref(body);
    if (raw)
    {
        ospfv3_install_and_flood_self(inst, area_id, raw, desired, now_msec);
    }
}

static void ospfv3_originate_link_lsa(ospfv3_instance_t *inst, const ospfv3_if_cfg_t *cfg,
                                      const if_api_cache_entry_t *entry, GHashTable *desired, uint64_t now_msec)
{
    if (cfg->passive || entry->ipv6_linklocal_addr.family != AF_INET6)
    {
        return;
    }
    GByteArray *body = g_byte_array_sized_new(48u);
    uint8_t header[24] = {0};
    header[0] = cfg->priority;
    ospfv3_put_u24(header + 1u, OSPFV3_OPTIONS_DEFAULT);
    memcpy(header + 4u, entry->ipv6_linklocal_addr.u.v6.s6_addr, 16u);
    g_byte_array_append(body, header, sizeof(header));
    uint32_t count = 0u;
    if (entry->ipv6_addr.family == AF_INET6 && !IN6_IS_ADDR_LINKLOCAL(&entry->ipv6_addr.u.v6))
    {
        ospfv3_append_prefix(body, &entry->ipv6_addr, entry->ipv6_prefix_len, 0u, 0u);
        count = 1u;
    }
    ospfv3_put_u32(body->data + 20u, count);
    GByteArray *raw = ospfv3_build_lsa(OSPFV3_LSA_LINK, entry->ifindex, inst->router_id, body->data, body->len);
    g_byte_array_unref(body);
    if (raw)
    {
        ospfv3_install_and_flood_self(inst, cfg->area_id, raw, desired, now_msec);
    }
}

static void ospfv3_originate_network_lsa(ospfv3_instance_t *inst, const ospfv3_if_cfg_t *cfg,
                                         const if_api_cache_entry_t *entry, GHashTable *desired, uint64_t now_msec)
{
    if (cfg->network_type != OSPFV3_NETWORK_BROADCAST || cfg->state != OSPFV3_IF_STATE_DR ||
        ospfv3_count_full_neighbors(inst, cfg) == 0u)
    {
        return;
    }
    GByteArray *body = g_byte_array_new();
    uint8_t options[4] = {0};
    ospfv3_put_u24(options + 1u, OSPFV3_OPTIONS_DEFAULT);
    g_byte_array_append(body, options, sizeof(options));
    uint8_t rid[4];
    ospfv3_put_u32(rid, inst->router_id);
    g_byte_array_append(body, rid, sizeof(rid));
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_neighbor_t *nbr = value;
        if (ospfv3_neighbor_is_full_on_if(nbr, cfg))
        {
            ospfv3_put_u32(rid, nbr->router_id);
            g_byte_array_append(body, rid, sizeof(rid));
        }
    }
    GByteArray *raw = ospfv3_build_lsa(OSPFV3_LSA_NETWORK, entry->ifindex, inst->router_id, body->data, body->len);
    g_byte_array_unref(body);
    if (raw)
    {
        ospfv3_install_and_flood_self(inst, cfg->area_id, raw, desired, now_msec);
    }
}

static gboolean ospfv3_lsa_flush_complete(const ospfv3_instance_t *inst, const ospfv3_lsa_entry_t *entry)
{
    char key[48];
    g_snprintf(key, sizeof(key), "%08x|%04x|%08x|%08x", entry->area_id, entry->type, entry->link_state_id,
               entry->advertising_router);
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_neighbor_t *nbr = value;
        if (nbr->area_id == entry->area_id &&
            ((nbr->state >= OSPFV3_NBR_STATE_EXSTART && nbr->state <= OSPFV3_NBR_STATE_LOADING) ||
             (nbr->retrans_lsas && g_hash_table_contains(nbr->retrans_lsas, key))))
        {
            return FALSE;
        }
    }
    return TRUE;
}

void ospfv3_lsa_originate_all(ospfv3_instance_t *inst, uint64_t now_msec)
{
    if (!inst || !inst->admin_up || inst->router_id == 0u)
    {
        return;
    }
    GHashTable *areas = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, NULL);
    GHashTable *desired = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!areas || !desired)
    {
        if (areas)
        {
            g_hash_table_destroy(areas);
        }
        if (desired)
        {
            g_hash_table_destroy(desired);
        }
        return;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_if_cfg_t *cfg = value;
        const if_api_cache_entry_t *entry = NULL;
        if (!ospfv3_if_is_ready(cfg, &entry))
        {
            continue;
        }
        uint32_t *area = g_new(uint32_t, 1);
        if (area)
        {
            *area = cfg->area_id;
            g_hash_table_add(areas, area);
        }
        ospfv3_originate_link_lsa(inst, cfg, entry, desired, now_msec);
        ospfv3_originate_network_lsa(inst, cfg, entry, desired, now_msec);
    }

    gpointer area_key = NULL;
    g_hash_table_iter_init(&iter, areas);
    while (g_hash_table_iter_next(&iter, &area_key, NULL))
    {
        uint32_t area_id = *(uint32_t *)area_key;
        ospfv3_originate_router_lsa(inst, area_id, desired, now_msec);
        ospfv3_originate_intra_prefix_lsa(inst, area_id, desired, now_msec);
    }

    g_hash_table_iter_init(&iter, inst->lsdb);
    gpointer key = NULL;
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        ospfv3_lsa_entry_t *entry = value;
        if (entry->self_originated && !g_hash_table_contains(desired, key))
        {
            ospfv3_lsa_mark_maxage(inst, entry, now_msec);
        }
    }
    inst->last_lsa_refresh_msec = now_msec;
    g_hash_table_destroy(desired);
    g_hash_table_destroy(areas);
}

void ospfv3_lsa_age(ospfv3_instance_t *inst, uint64_t now_msec)
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
        ospfv3_lsa_entry_t *entry = value;
        uint16_t age = ospfv3_lsa_current_age(entry, now_msec);
        if (age >= OSPFV3_LSA_MAX_AGE)
        {
            if (entry->age != OSPFV3_LSA_MAX_AGE)
            {
                ospfv3_lsa_mark_maxage(inst, entry, now_msec);
            }
            if (ospfv3_lsa_flush_complete(inst, entry))
            {
                refresh |= entry->self_originated != 0u;
                g_hash_table_iter_remove(&iter);
                changed = TRUE;
            }
        }
        else if (entry->self_originated && age >= OSPFV3_LSA_REFRESH_TIME)
        {
            refresh = TRUE;
        }
    }
    gboolean pending = inst->next_lsa_originate_msec && now_msec >= inst->next_lsa_originate_msec;
    if (pending)
    {
        inst->next_lsa_originate_msec = 0u;
    }
    if (refresh || pending)
    {
        ospfv3_lsa_originate_all(inst, now_msec);
    }
    if (changed || refresh || pending)
    {
        ospfv3_spf_recalculate(inst);
    }
}

void ospfv3_lsa_flush_self(ospfv3_instance_t *inst)
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
        ospfv3_lsa_entry_t *entry = value;
        if (entry->self_originated)
        {
            ospfv3_lsa_mark_maxage(inst, entry, ospfv3_now_msec());
            g_hash_table_iter_remove(&iter);
        }
    }
}
