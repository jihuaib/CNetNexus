/**
 * @file   ospfv3_packet.c
 * @brief  OSPFv3 packet I/O, neighbor discovery, and database exchange
 */
#include "ospfv3_packet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/ip6.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "if.h"
#include "log.h"
#include "ospfv3_lsa.h"
#include "ospfv3_spf.h"

#define OSPFV3_DBD_FLAG_MS 0x01u
#define OSPFV3_DBD_FLAG_M 0x02u
#define OSPFV3_DBD_FLAG_I 0x04u
#define OSPFV3_PACKET_BUFFER_SIZE 65535u
#define OSPFV3_DEFAULT_INTERFACE_MTU 1500u
#define OSPFV3_RETRANSMIT_INTERVAL_MSEC 3000u

typedef struct ospfv3_lsa_request
{
    uint32_t link_state_id;
    uint32_t advertising_router;
    uint16_t type;
} ospfv3_lsa_request_t;

typedef struct ospfv3_lsa_retrans
{
    GByteArray *raw;
    uint64_t installed_msec;
} ospfv3_lsa_retrans_t;

typedef struct ospfv3_dbd_summary
{
    uint8_t header[OSPFV3_LSA_HEADER_LEN];
} ospfv3_dbd_summary_t;

typedef struct ospfv3_packet_context
{
    ospfv3_instance_t *inst;
    ospfv3_if_cfg_t *cfg;
    const if_api_cache_entry_t *if_entry;
} ospfv3_packet_context_t;

static const struct in6_addr OSPFV3_ALL_SPF_ROUTERS = {
    .s6_addr = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5},
};
static const struct in6_addr OSPFV3_ALL_DR_ROUTERS = {
    .s6_addr = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6},
};

static void ospfv3_lsa_retrans_free(gpointer data)
{
    ospfv3_lsa_retrans_t *item = (ospfv3_lsa_retrans_t *)data;
    if (item)
    {
        if (item->raw)
        {
            g_byte_array_unref(item->raw);
        }
        g_free(item);
    }
}

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

static uint32_t ospfv3_get_u24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16u) | ((uint32_t)p[1] << 8u) | p[2];
}

static void ospfv3_put_u24(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 16u);
    p[1] = (uint8_t)(value >> 8u);
    p[2] = (uint8_t)value;
}

static void ospfv3_format_ipv4(uint32_t address, char *text, size_t text_len)
{
    struct in_addr addr = {.s_addr = htonl(address)};
    if (!inet_ntop(AF_INET, &addr, text, text_len))
    {
        g_strlcpy(text, "0.0.0.0", text_len);
    }
}

static uint32_t ospfv3_internet_checksum_add(uint32_t sum, const uint8_t *data, size_t len)
{
    while (len >= 2u)
    {
        sum += ((uint32_t)data[0] << 8u) | data[1];
        data += 2u;
        len -= 2u;
    }
    if (len != 0u)
    {
        sum += (uint32_t)data[0] << 8u;
    }
    return sum;
}

static uint16_t ospfv3_internet_checksum_finish(uint32_t sum)
{
    while ((sum >> 16u) != 0u)
    {
        sum = (sum & 0xffffu) + (sum >> 16u);
    }
    return (uint16_t)(~sum & 0xffffu);
}

uint16_t ospfv3_internet_checksum(const uint8_t *data, size_t len)
{
    return ospfv3_internet_checksum_finish(ospfv3_internet_checksum_add(0u, data, len));
}

static uint16_t ospfv3_packet_checksum(const struct in6_addr *source, const struct in6_addr *destination,
                                       const uint8_t *packet, size_t len)
{
    if (!source || !destination || !packet || len < OSPFV3_HEADER_LEN || len > UINT32_MAX)
    {
        return UINT16_MAX;
    }

    uint32_t sum = ospfv3_internet_checksum_add(0u, source->s6_addr, sizeof(source->s6_addr));
    sum = ospfv3_internet_checksum_add(sum, destination->s6_addr, sizeof(destination->s6_addr));
    uint8_t pseudo_tail[8] = {0};
    ospfv3_put_u32(pseudo_tail, (uint32_t)len);
    pseudo_tail[7] = OSPFV3_IP_PROTOCOL;
    sum = ospfv3_internet_checksum_add(sum, pseudo_tail, sizeof(pseudo_tail));
    sum = ospfv3_internet_checksum_add(sum, packet, len);
    return ospfv3_internet_checksum_finish(sum);
}

uint16_t ospfv3_lsa_checksum(uint8_t *lsa, size_t len)
{
    if (!lsa || len < OSPFV3_LSA_HEADER_LEN || len > UINT16_MAX)
    {
        return 0u;
    }

    lsa[16] = 0u;
    lsa[17] = 0u;
    uint32_t c0 = 0u;
    uint32_t c1 = 0u;
    for (size_t i = 2u; i < len; ++i)
    {
        c0 = (c0 + lsa[i]) % 255u;
        c1 = (c1 + c0) % 255u;
    }

    int32_t x = (int32_t)(((len - 17u) * c0) % 255u) - (int32_t)c1;
    x %= 255;
    if (x <= 0)
    {
        x += 255;
    }
    int32_t y = 510 - (int32_t)c0 - x;
    if (y > 255)
    {
        y -= 255;
    }
    return (uint16_t)(((uint16_t)x << 8u) | (uint16_t)y);
}

int ospfv3_lsa_checksum_valid(const uint8_t *lsa, size_t len)
{
    if (!lsa || len < OSPFV3_LSA_HEADER_LEN)
    {
        return 0;
    }

    uint32_t c0 = 0u;
    uint32_t c1 = 0u;
    for (size_t i = 2u; i < len; ++i)
    {
        c0 = (c0 + lsa[i]) % 255u;
        c1 = (c1 + c0) % 255u;
    }
    return c0 == 0u && c1 == 0u;
}

static gboolean ospfv3_interface_ready(const ospfv3_if_cfg_t *cfg, const if_api_cache_entry_t **if_entry_out)
{
    const if_api_cache_entry_t *if_entry = cfg ? if_api_cache_lookup(cfg->ifname) : NULL;
    if (if_entry_out)
    {
        *if_entry_out = if_entry;
    }
    return cfg && cfg->enabled && !cfg->passive && ospfv3_if_entry_matches_vrf(cfg->vrf_name, if_entry) &&
           if_entry->ifindex != 0u && if_entry->link_up && if_entry->ipv6_linklocal_addr.family == AF_INET6;
}

static uint16_t ospfv3_interface_mtu(const ospfv3_if_cfg_t *cfg)
{
    const if_api_cache_entry_t *if_entry = cfg ? if_api_cache_lookup(cfg->ifname) : NULL;
    if (!if_entry || !g_ospfv3_work_local || g_ospfv3_work_local->raw_fd < 0)
    {
        return OSPFV3_DEFAULT_INTERFACE_MTU;
    }

    const char *ifname = if_entry->physical_name[0] != '\0' ? if_entry->physical_name : if_entry->logical_name;
    struct ifreq request;
    memset(&request, 0, sizeof(request));
    g_strlcpy(request.ifr_name, ifname, sizeof(request.ifr_name));
    if (ioctl(g_ospfv3_work_local->raw_fd, SIOCGIFMTU, &request) < 0 || request.ifr_mtu <= 0)
    {
        return OSPFV3_DEFAULT_INTERFACE_MTU;
    }
    if ((uint32_t)request.ifr_mtu > UINT16_MAX)
    {
        return UINT16_MAX;
    }
    return (uint16_t)request.ifr_mtu;
}

static ospfv3_neighbor_t *ospfv3_neighbor_lookup(ospfv3_instance_t *inst, const char *ifname, uint32_t router_id)
{
    char key[IF_LOGICAL_NAME_MAX + 16u];
    g_snprintf(key, sizeof(key), "%s|%08x", ifname, router_id);
    return (ospfv3_neighbor_t *)g_hash_table_lookup(inst->neighbors, key);
}

static ospfv3_neighbor_t *ospfv3_neighbor_get_or_create(ospfv3_instance_t *inst, const ospfv3_if_cfg_t *cfg,
                                                        uint32_t router_id)
{
    ospfv3_neighbor_t *nbr = ospfv3_neighbor_lookup(inst, cfg->ifname, router_id);
    if (nbr)
    {
        return nbr;
    }

    nbr = g_malloc0(sizeof(*nbr));
    char *key = g_strdup_printf("%s|%08x", cfg->ifname, router_id);
    if (!nbr || !key)
    {
        g_free(nbr);
        g_free(key);
        return NULL;
    }
    g_strlcpy(nbr->ifname, cfg->ifname, sizeof(nbr->ifname));
    nbr->area_id = cfg->area_id;
    nbr->router_id = router_id;
    nbr->state = OSPFV3_NBR_STATE_DOWN;
    nbr->request_keys = g_ptr_array_new_with_free_func(g_free);
    nbr->dd_summaries = g_ptr_array_new_with_free_func(g_free);
    nbr->retrans_lsas = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ospfv3_lsa_retrans_free);
    if (!nbr->request_keys || !nbr->dd_summaries || !nbr->retrans_lsas)
    {
        if (nbr->request_keys)
        {
            g_ptr_array_free(nbr->request_keys, TRUE);
        }
        if (nbr->dd_summaries)
        {
            g_ptr_array_free(nbr->dd_summaries, TRUE);
        }
        if (nbr->retrans_lsas)
        {
            g_hash_table_destroy(nbr->retrans_lsas);
        }
        g_free(nbr);
        g_free(key);
        return NULL;
    }
    g_hash_table_insert(inst->neighbors, key, nbr);
    return nbr;
}

static gboolean ospfv3_neighbor_has_parallel_adjacency(const ospfv3_instance_t *inst, const ospfv3_neighbor_t *current)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_neighbor_t *nbr = (const ospfv3_neighbor_t *)value;
        if (nbr != current && nbr->router_id == current->router_id && nbr->state >= OSPFV3_NBR_STATE_TWO_WAY &&
            strcmp(nbr->ifname, current->ifname) != 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void ospfv3_neighbor_clear_exchange(ospfv3_neighbor_t *nbr)
{
    if (!nbr)
    {
        return;
    }
    nbr->dd_sequence = 0u;
    nbr->dd_master = 0u;
    nbr->dd_summary_cursor = 0u;
    nbr->dd_peer_mtu = 0u;
    nbr->dd_peer_more = 0u;
    nbr->dd_local_more = 0u;
    nbr->dd_options = 0u;
    nbr->last_dbd_tx_msec = 0u;
    nbr->last_lsa_retransmit_msec = 0u;
    if (nbr->request_keys)
    {
        g_ptr_array_set_size(nbr->request_keys, 0u);
    }
    if (nbr->dd_summaries)
    {
        g_ptr_array_set_size(nbr->dd_summaries, 0u);
    }
    if (nbr->last_dbd_tx)
    {
        g_byte_array_unref(nbr->last_dbd_tx);
        nbr->last_dbd_tx = NULL;
    }
    if (nbr->last_dbd_rx)
    {
        g_byte_array_unref(nbr->last_dbd_rx);
        nbr->last_dbd_rx = NULL;
    }
    if (nbr->retrans_lsas)
    {
        g_hash_table_remove_all(nbr->retrans_lsas);
    }
}

static int ospfv3_set_membership(const ospfv3_if_cfg_t *cfg, const struct in6_addr *group, int join)
{
    const if_api_cache_entry_t *if_entry = NULL;
    if (!g_ospfv3_work_local || g_ospfv3_work_local->raw_fd < 0 || !ospfv3_interface_ready(cfg, &if_entry))
    {
        return -1;
    }

    struct ipv6_mreq request;
    memset(&request, 0, sizeof(request));
    request.ipv6mr_multiaddr = *group;
    request.ipv6mr_interface = if_entry->ifindex;
    int option = join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP;
    if (setsockopt(g_ospfv3_work_local->raw_fd, IPPROTO_IPV6, option, &request, sizeof(request)) == 0)
    {
        return 0;
    }
    if ((join && errno == EADDRINUSE) || (!join && (errno == EADDRNOTAVAIL || errno == ENODEV)))
    {
        return 0;
    }
    LOG_WARN("OSPFV3: %s multicast group on %s failed: %s", join ? "join" : "leave", cfg->ifname, g_strerror(errno));
    return -1;
}

static int ospfv3_send_raw(const ospfv3_if_cfg_t *cfg, const struct in6_addr *destination, uint8_t *packet,
                           size_t packet_len)
{
    const if_api_cache_entry_t *if_entry = NULL;
    if (!packet || packet_len < OSPFV3_HEADER_LEN || !ospfv3_interface_ready(cfg, &if_entry) || !g_ospfv3_work_local ||
        g_ospfv3_work_local->raw_fd < 0)
    {
        return -1;
    }

    struct sockaddr_in6 dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin6_family = AF_INET6;
    dst.sin6_addr = *destination;
    dst.sin6_scope_id = if_entry->ifindex;

    ospfv3_put_u16(packet + 12u, 0u);
    uint16_t checksum = ospfv3_packet_checksum(&if_entry->ipv6_linklocal_addr.u.v6, destination, packet, packet_len);
    ospfv3_put_u16(packet + 12u, checksum);

    struct iovec iov = {.iov_base = (void *)packet, .iov_len = packet_len};
    uint8_t control[CMSG_SPACE(sizeof(struct in6_pktinfo))];
    memset(control, 0, sizeof(control));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &dst;
    msg.msg_namelen = sizeof(dst);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1u;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = IPPROTO_IPV6;
    cmsg->cmsg_type = IPV6_PKTINFO;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
    struct in6_pktinfo *pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);
    memset(pktinfo, 0, sizeof(*pktinfo));
    pktinfo->ipi6_ifindex = if_entry->ifindex;
    pktinfo->ipi6_addr = if_entry->ipv6_linklocal_addr.u.v6;

    ssize_t sent = sendmsg(g_ospfv3_work_local->raw_fd, &msg, MSG_DONTWAIT);
    if (sent == (ssize_t)packet_len)
    {
        return 0;
    }
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_WARN("OSPFV3: packet send on %s failed: %s", cfg->ifname, g_strerror(errno));
    }
    return -1;
}

static GByteArray *ospfv3_packet_build(const ospfv3_instance_t *inst, const ospfv3_if_cfg_t *cfg, uint8_t type,
                                       const uint8_t *body, size_t body_len)
{
    size_t packet_len = OSPFV3_HEADER_LEN + body_len;
    if (!inst || !cfg || packet_len > UINT16_MAX)
    {
        return NULL;
    }

    GByteArray *packet = g_byte_array_sized_new(packet_len);
    if (!packet)
    {
        return NULL;
    }
    g_byte_array_set_size(packet, packet_len);
    memset(packet->data, 0, packet_len);
    packet->data[0] = OSPFV3_VERSION;
    packet->data[1] = type;
    ospfv3_put_u16(packet->data + 2u, (uint16_t)packet_len);
    ospfv3_put_u32(packet->data + 4u, inst->router_id);
    ospfv3_put_u32(packet->data + 8u, cfg->area_id);
    packet->data[14] = OSPFV3_INSTANCE_ID;
    packet->data[15] = 0u;
    if (body_len != 0u)
    {
        memcpy(packet->data + OSPFV3_HEADER_LEN, body, body_len);
    }
    return packet;
}

static void ospfv3_lsa_copy_current_header(uint8_t *header, const ospfv3_lsa_entry_t *entry, uint64_t now_msec)
{
    memcpy(header, entry->raw->data, OSPFV3_LSA_HEADER_LEN);
    uint64_t elapsed = now_msec >= entry->installed_msec ? (now_msec - entry->installed_msec) / 1000u : 0u;
    uint64_t age = (uint64_t)entry->age + elapsed;
    ospfv3_put_u16(header, age >= OSPFV3_LSA_MAX_AGE ? OSPFV3_LSA_MAX_AGE : (uint16_t)age);
}

static void ospfv3_lsa_append_current(GByteArray *body, const ospfv3_lsa_entry_t *entry, uint64_t now_msec)
{
    size_t offset = body->len;
    g_byte_array_append(body, entry->raw->data, entry->raw->len);
    uint64_t elapsed = now_msec >= entry->installed_msec ? (now_msec - entry->installed_msec) / 1000u : 0u;
    uint64_t age = (uint64_t)entry->age + elapsed;
    ospfv3_put_u16(body->data + offset, age >= OSPFV3_LSA_MAX_AGE ? OSPFV3_LSA_MAX_AGE : (uint16_t)age);
}

static int ospfv3_send_hello(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, uint64_t now_msec)
{
    const if_api_cache_entry_t *if_entry = NULL;
    if (!ospfv3_interface_ready(cfg, &if_entry))
    {
        return -1;
    }

    GByteArray *body = g_byte_array_sized_new(64u);
    if (!body)
    {
        return -1;
    }
    uint8_t hello[20] = {0};
    ospfv3_put_u32(hello, if_entry->ifindex);
    hello[4] = cfg->priority;
    ospfv3_put_u24(hello + 5u, OSPFV3_OPTIONS_DEFAULT);
    ospfv3_put_u16(hello + 8u, cfg->hello_interval);
    ospfv3_put_u16(hello + 10u, (uint16_t)cfg->dead_interval);
    ospfv3_put_u32(hello + 12u, cfg->dr);
    ospfv3_put_u32(hello + 16u, cfg->bdr);
    g_byte_array_append(body, hello, sizeof(hello));

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_neighbor_t *nbr = (const ospfv3_neighbor_t *)value;
        if (nbr->state >= OSPFV3_NBR_STATE_INIT && nbr->area_id == cfg->area_id &&
            strcmp(nbr->ifname, cfg->ifname) == 0)
        {
            uint8_t router_id[4];
            ospfv3_put_u32(router_id, nbr->router_id);
            g_byte_array_append(body, router_id, sizeof(router_id));
        }
    }

    GByteArray *packet = ospfv3_packet_build(inst, cfg, OSPFV3_PACKET_HELLO, body->data, body->len);
    g_byte_array_unref(body);
    if (!packet)
    {
        return -1;
    }
    int rc = ospfv3_send_raw(cfg, &OSPFV3_ALL_SPF_ROUTERS, packet->data, packet->len);
    g_byte_array_unref(packet);
    if (rc == 0)
    {
        cfg->last_hello_tx_msec = now_msec;
    }
    return rc;
}

static gint ospfv3_dbd_summary_compare(gconstpointer a, gconstpointer b)
{
    const ospfv3_dbd_summary_t *left = *(const ospfv3_dbd_summary_t *const *)a;
    const ospfv3_dbd_summary_t *right = *(const ospfv3_dbd_summary_t *const *)b;
    uint16_t left_type = ospfv3_get_u16(left->header + 2u);
    uint16_t right_type = ospfv3_get_u16(right->header + 2u);
    if (left_type != right_type)
    {
        return left_type < right_type ? -1 : 1;
    }
    uint32_t left_id = ospfv3_get_u32(left->header + 4u);
    uint32_t right_id = ospfv3_get_u32(right->header + 4u);
    if (left_id != right_id)
    {
        return left_id < right_id ? -1 : 1;
    }
    uint32_t left_router = ospfv3_get_u32(left->header + 8u);
    uint32_t right_router = ospfv3_get_u32(right->header + 8u);
    if (left_router != right_router)
    {
        return left_router < right_router ? -1 : 1;
    }
    return memcmp(left->header, right->header, OSPFV3_LSA_HEADER_LEN);
}

static gboolean ospfv3_neighbor_snapshot_summaries(const ospfv3_instance_t *inst, ospfv3_neighbor_t *nbr)
{
    if (!inst || !nbr || !nbr->dd_summaries)
    {
        return FALSE;
    }

    g_ptr_array_set_size(nbr->dd_summaries, 0u);
    nbr->dd_summary_cursor = 0u;
    uint64_t now_msec = ospfv3_now_msec();
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_lsa_entry_t *entry = (const ospfv3_lsa_entry_t *)value;
        if (entry->area_id != nbr->area_id || !entry->raw || entry->raw->len < OSPFV3_LSA_HEADER_LEN)
        {
            continue;
        }
        ospfv3_dbd_summary_t *summary = g_malloc(sizeof(*summary));
        if (!summary)
        {
            g_ptr_array_set_size(nbr->dd_summaries, 0u);
            return FALSE;
        }
        ospfv3_lsa_copy_current_header(summary->header, entry, now_msec);
        g_ptr_array_add(nbr->dd_summaries, summary);
    }
    g_ptr_array_sort(nbr->dd_summaries, ospfv3_dbd_summary_compare);
    return TRUE;
}

static gboolean ospfv3_cache_received_dbd(ospfv3_neighbor_t *nbr, const uint8_t *body, size_t body_len)
{
    GByteArray *copy = g_byte_array_sized_new(body_len);
    if (!copy)
    {
        return FALSE;
    }
    g_byte_array_append(copy, body, body_len);
    if (nbr->last_dbd_rx)
    {
        g_byte_array_unref(nbr->last_dbd_rx);
    }
    nbr->last_dbd_rx = copy;
    return TRUE;
}

static gboolean ospfv3_received_dbd_is_duplicate(const ospfv3_neighbor_t *nbr, const uint8_t *body, size_t body_len)
{
    return nbr && nbr->last_dbd_rx && nbr->last_dbd_rx->len == body_len &&
           memcmp(nbr->last_dbd_rx->data, body, body_len) == 0;
}

static int ospfv3_retransmit_last_dbd(const ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr)
{
    if (!cfg || !nbr || !nbr->last_dbd_tx || nbr->last_dbd_tx->len < OSPFV3_HEADER_LEN + 12u)
    {
        return -1;
    }
    int rc = ospfv3_send_raw(cfg, &nbr->src_addr, nbr->last_dbd_tx->data, nbr->last_dbd_tx->len);
    if (rc == 0)
    {
        nbr->last_dbd_tx_msec = ospfv3_now_msec();
    }
    return rc;
}

static int ospfv3_send_dbd(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr, uint8_t flags,
                           int include_summaries)
{
    uint16_t interface_mtu = ospfv3_interface_mtu(cfg);
    uint16_t packet_mtu = nbr->dd_peer_mtu != 0u && nbr->dd_peer_mtu < interface_mtu ? nbr->dd_peer_mtu : interface_mtu;
    if (packet_mtu < sizeof(struct ip6_hdr) + OSPFV3_HEADER_LEN + 12u)
    {
        return -1;
    }
    size_t max_ospfv3_len = (size_t)packet_mtu - sizeof(struct ip6_hdr);

    GByteArray *body = g_byte_array_sized_new(128u);
    if (!body)
    {
        return -1;
    }
    uint8_t dbd[12] = {0};
    ospfv3_put_u24(dbd + 1u, OSPFV3_OPTIONS_DEFAULT);
    ospfv3_put_u16(dbd + 4u, interface_mtu);
    dbd[7] = flags;
    ospfv3_put_u32(dbd + 8u, nbr->dd_sequence);
    g_byte_array_append(body, dbd, sizeof(dbd));

    uint32_t page_start = nbr->dd_summary_cursor;
    uint8_t previous_local_more = nbr->dd_local_more;
    if (include_summaries)
    {
        body->data[7] &= (uint8_t)~OSPFV3_DBD_FLAG_M;
        while (nbr->dd_summaries && nbr->dd_summary_cursor < nbr->dd_summaries->len &&
               OSPFV3_HEADER_LEN + body->len + OSPFV3_LSA_HEADER_LEN <= max_ospfv3_len)
        {
            const ospfv3_dbd_summary_t *summary = g_ptr_array_index(nbr->dd_summaries, nbr->dd_summary_cursor);
            g_byte_array_append(body, summary->header, OSPFV3_LSA_HEADER_LEN);
            nbr->dd_summary_cursor++;
        }
        if (nbr->dd_summaries && nbr->dd_summary_cursor < nbr->dd_summaries->len)
        {
            body->data[7] |= OSPFV3_DBD_FLAG_M;
        }
        if (nbr->dd_summary_cursor == page_start && (body->data[7] & OSPFV3_DBD_FLAG_M) != 0u)
        {
            g_byte_array_unref(body);
            return -1;
        }
    }
    nbr->dd_local_more = (body->data[7] & OSPFV3_DBD_FLAG_M) != 0u;

    GByteArray *packet = ospfv3_packet_build(inst, cfg, OSPFV3_PACKET_DBD, body->data, body->len);
    g_byte_array_unref(body);
    if (!packet)
    {
        nbr->dd_summary_cursor = page_start;
        nbr->dd_local_more = previous_local_more;
        return -1;
    }
    if (nbr->last_dbd_tx)
    {
        g_byte_array_unref(nbr->last_dbd_tx);
    }
    nbr->last_dbd_tx = g_byte_array_ref(packet);
    int rc = ospfv3_send_raw(cfg, &nbr->src_addr, packet->data, packet->len);
    g_byte_array_unref(packet);
    if (rc == 0)
    {
        nbr->last_dbd_tx_msec = ospfv3_now_msec();
    }
    return rc;
}

static int ospfv3_send_lsr(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr)
{
    if (!nbr->request_keys || nbr->request_keys->len == 0u)
    {
        return 0;
    }
    GByteArray *body = g_byte_array_sized_new(nbr->request_keys->len * 12u);
    if (!body)
    {
        return -1;
    }
    for (guint i = 0u; i < nbr->request_keys->len; ++i)
    {
        const ospfv3_lsa_request_t *request = g_ptr_array_index(nbr->request_keys, i);
        uint8_t item[12];
        ospfv3_put_u32(item, request->type);
        ospfv3_put_u32(item + 4u, request->link_state_id);
        ospfv3_put_u32(item + 8u, request->advertising_router);
        g_byte_array_append(body, item, sizeof(item));
    }
    GByteArray *packet = ospfv3_packet_build(inst, cfg, OSPFV3_PACKET_LS_REQUEST, body->data, body->len);
    g_byte_array_unref(body);
    if (!packet)
    {
        return -1;
    }
    int rc = ospfv3_send_raw(cfg, &nbr->src_addr, packet->data, packet->len);
    g_byte_array_unref(packet);
    if (rc == 0)
    {
        nbr->last_dbd_tx_msec = ospfv3_now_msec();
    }
    return rc;
}

static int ospfv3_send_lsu_body(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr,
                                GByteArray *lsa_body, uint32_t count)
{
    GByteArray *body = g_byte_array_sized_new(4u + lsa_body->len);
    if (!body)
    {
        return -1;
    }
    uint8_t count_bytes[4];
    ospfv3_put_u32(count_bytes, count);
    g_byte_array_append(body, count_bytes, sizeof(count_bytes));
    g_byte_array_append(body, lsa_body->data, lsa_body->len);
    GByteArray *packet = ospfv3_packet_build(inst, cfg, OSPFV3_PACKET_LS_UPDATE, body->data, body->len);
    g_byte_array_unref(body);
    if (!packet)
    {
        return -1;
    }
    int rc = ospfv3_send_raw(cfg, &nbr->src_addr, packet->data, packet->len);
    g_byte_array_unref(packet);
    return rc;
}

static gboolean ospfv3_retransmit_enqueue(ospfv3_neighbor_t *nbr, const ospfv3_lsa_entry_t *entry)
{
    if (!nbr || !nbr->retrans_lsas || !entry || !entry->raw || entry->raw->len < OSPFV3_LSA_HEADER_LEN)
    {
        return FALSE;
    }

    char *key = ospfv3_lsa_key_new(nbr->area_id, entry->type, entry->link_state_id, entry->advertising_router);
    ospfv3_lsa_retrans_t *item = g_malloc0(sizeof(*item));
    if (!key || !item)
    {
        g_free(key);
        g_free(item);
        return FALSE;
    }
    item->raw = g_byte_array_sized_new(entry->raw->len);
    if (!item->raw)
    {
        g_free(key);
        g_free(item);
        return FALSE;
    }
    g_byte_array_append(item->raw, entry->raw->data, entry->raw->len);
    item->installed_msec = entry->installed_msec;
    g_hash_table_replace(nbr->retrans_lsas, key, item);
    return TRUE;
}

static int ospfv3_send_lsa_entry(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr,
                                 const ospfv3_lsa_entry_t *entry, gboolean track_retransmit)
{
    if (!inst || !cfg || !nbr || !entry || !entry->raw)
    {
        return -1;
    }

    uint64_t now_msec = ospfv3_now_msec();
    if (track_retransmit)
    {
        (void)ospfv3_retransmit_enqueue(nbr, entry);
    }

    GByteArray *body = g_byte_array_sized_new(entry->raw->len);
    if (!body)
    {
        return -1;
    }
    ospfv3_lsa_append_current(body, entry, now_msec);
    int rc = ospfv3_send_lsu_body(inst, cfg, nbr, body, 1u);
    g_byte_array_unref(body);
    if (track_retransmit && rc == 0)
    {
        nbr->last_lsa_retransmit_msec = now_msec;
    }
    return rc;
}

static int ospfv3_send_retransmit_item(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr,
                                       const ospfv3_lsa_retrans_t *item, uint64_t now_msec)
{
    if (!item || !item->raw || item->raw->len < OSPFV3_LSA_HEADER_LEN)
    {
        return -1;
    }

    GByteArray *body = g_byte_array_sized_new(item->raw->len);
    if (!body)
    {
        return -1;
    }
    g_byte_array_append(body, item->raw->data, item->raw->len);
    uint64_t elapsed = now_msec >= item->installed_msec ? (now_msec - item->installed_msec) / 1000u : 0u;
    uint64_t age = (uint64_t)ospfv3_get_u16(item->raw->data) + elapsed;
    ospfv3_put_u16(body->data, age >= OSPFV3_LSA_MAX_AGE ? OSPFV3_LSA_MAX_AGE : (uint16_t)age);
    int rc = ospfv3_send_lsu_body(inst, cfg, nbr, body, 1u);
    g_byte_array_unref(body);
    return rc;
}

static gboolean ospfv3_retransmit_header_matches(const ospfv3_lsa_retrans_t *item, const uint8_t *header,
                                                 gboolean accept_newer)
{
    if (!item || !item->raw || item->raw->len < OSPFV3_LSA_HEADER_LEN || !header)
    {
        return FALSE;
    }

    int32_t candidate_sequence = (int32_t)ospfv3_get_u32(header + 12u);
    int32_t queued_sequence = (int32_t)ospfv3_get_u32(item->raw->data + 12u);
    uint16_t candidate_checksum = ospfv3_get_u16(header + 16u);
    uint16_t queued_checksum = ospfv3_get_u16(item->raw->data + 16u);
    if (!accept_newer)
    {
        return candidate_sequence == queued_sequence && candidate_checksum == queued_checksum;
    }
    return candidate_sequence > queued_sequence ||
           (candidate_sequence == queued_sequence && candidate_checksum >= queued_checksum);
}

static void ospfv3_retransmit_acknowledge(ospfv3_neighbor_t *nbr, const uint8_t *header, gboolean accept_newer)
{
    if (!nbr || !nbr->retrans_lsas || !header)
    {
        return;
    }

    char *key = ospfv3_lsa_key_new(nbr->area_id, ospfv3_get_u16(header + 2u), ospfv3_get_u32(header + 4u),
                                   ospfv3_get_u32(header + 8u));
    if (!key)
    {
        return;
    }
    ospfv3_lsa_retrans_t *item = g_hash_table_lookup(nbr->retrans_lsas, key);
    if (ospfv3_retransmit_header_matches(item, header, accept_newer))
    {
        g_hash_table_remove(nbr->retrans_lsas, key);
    }
    g_free(key);
}

static void ospfv3_retransmit_pending(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr,
                                      uint64_t now_msec)
{
    if (!inst || !cfg || !nbr || nbr->state < OSPFV3_NBR_STATE_EXCHANGE || !nbr->retrans_lsas ||
        g_hash_table_size(nbr->retrans_lsas) == 0u)
    {
        return;
    }
    if (nbr->last_lsa_retransmit_msec != 0u && now_msec >= nbr->last_lsa_retransmit_msec &&
        now_msec - nbr->last_lsa_retransmit_msec < OSPFV3_RETRANSMIT_INTERVAL_MSEC)
    {
        return;
    }

    gboolean sent = FALSE;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, nbr->retrans_lsas);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        if (ospfv3_send_retransmit_item(inst, cfg, nbr, (const ospfv3_lsa_retrans_t *)value, now_msec) == 0)
        {
            sent = TRUE;
        }
    }
    if (sent)
    {
        nbr->last_lsa_retransmit_msec = now_msec;
    }
}

static int ospfv3_send_ack(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr,
                           const GByteArray *headers)
{
    if (!headers || headers->len == 0u)
    {
        return 0;
    }
    GByteArray *packet = ospfv3_packet_build(inst, cfg, OSPFV3_PACKET_LS_ACK, headers->data, headers->len);
    if (!packet)
    {
        return -1;
    }
    int rc = ospfv3_send_raw(cfg, &nbr->src_addr, packet->data, packet->len);
    g_byte_array_unref(packet);
    return rc;
}

void ospfv3_packet_send_lsdb(ospfv3_instance_t *inst, ospfv3_neighbor_t *nbr)
{
    if (!inst || !nbr)
    {
        return;
    }
    ospfv3_if_cfg_t *cfg = g_hash_table_lookup(inst->if_cfgs, nbr->ifname);
    if (!cfg || nbr->state < OSPFV3_NBR_STATE_EXCHANGE)
    {
        return;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->lsdb);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_lsa_entry_t *entry = (const ospfv3_lsa_entry_t *)value;
        if (entry->area_id != nbr->area_id || !entry->raw)
        {
            continue;
        }
        (void)ospfv3_send_lsa_entry(inst, cfg, nbr, entry, TRUE);
    }
}

void ospfv3_packet_flood_lsa(ospfv3_instance_t *inst, const ospfv3_lsa_entry_t *entry, const ospfv3_neighbor_t *exclude)
{
    if (!inst || !entry || !entry->raw)
    {
        return;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        ospfv3_neighbor_t *nbr = (ospfv3_neighbor_t *)value;
        ospfv3_if_cfg_t *cfg = g_hash_table_lookup(inst->if_cfgs, nbr->ifname);
        if (nbr == exclude || nbr->state < OSPFV3_NBR_STATE_EXCHANGE || nbr->area_id != entry->area_id || !cfg)
        {
            continue;
        }
        (void)ospfv3_send_lsa_entry(inst, cfg, nbr, entry, TRUE);
    }
}

static gboolean ospfv3_adjacency_needed(const ospfv3_instance_t *inst, const ospfv3_if_cfg_t *cfg,
                                        const ospfv3_neighbor_t *nbr)
{
    (void)inst;
    if (cfg->network_type == OSPFV3_NETWORK_POINT_TO_POINT)
    {
        return TRUE;
    }
    return cfg->dr == inst->router_id || cfg->bdr == inst->router_id || cfg->dr == nbr->router_id ||
           cfg->bdr == nbr->router_id;
}

static void ospfv3_neighbor_begin_exchange(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr,
                                           int force)
{
    if (!inst || !cfg || !nbr || (!force && nbr->state >= OSPFV3_NBR_STATE_EXSTART))
    {
        return;
    }

    uint32_t previous_sequence = nbr->dd_sequence;
    ospfv3_neighbor_clear_exchange(nbr);
    nbr->state = OSPFV3_NBR_STATE_EXSTART;
    nbr->dd_master = 1u;
    nbr->dd_sequence =
        previous_sequence != 0u ? previous_sequence + 1u : (uint32_t)(ospfv3_now_msec() ^ inst->router_id);
    if (nbr->dd_sequence == 0u)
    {
        nbr->dd_sequence = 1u;
    }
    if (!ospfv3_neighbor_snapshot_summaries(inst, nbr))
    {
        nbr->state = OSPFV3_NBR_STATE_TWO_WAY;
        return;
    }
    (void)ospfv3_send_dbd(inst, cfg, nbr, OSPFV3_DBD_FLAG_I | OSPFV3_DBD_FLAG_M | OSPFV3_DBD_FLAG_MS, 0);
}

static void ospfv3_neighbor_start_exchange(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr)
{
    ospfv3_neighbor_begin_exchange(inst, cfg, nbr, 0);
}

static void ospfv3_neighbor_restart_exchange(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr)
{
    gboolean topology_changed = nbr && nbr->state == OSPFV3_NBR_STATE_FULL;
    ospfv3_neighbor_begin_exchange(inst, cfg, nbr, 1);
    if (topology_changed)
    {
        ospfv3_lsa_originate_all(inst, ospfv3_now_msec());
        ospfv3_spf_recalculate(inst);
    }
}

typedef struct ospfv3_dr_candidate
{
    uint32_t router_id;
    uint32_t address;
    uint8_t priority;
    uint8_t declared_dr;
    uint8_t declared_bdr;
} ospfv3_dr_candidate_t;

typedef enum ospfv3_dr_candidate_class
{
    OSPFV3_DR_CANDIDATE_DECLARED_DR = 1,
    OSPFV3_DR_CANDIDATE_DECLARED_BDR = 2,
    OSPFV3_DR_CANDIDATE_NOT_DECLARED_DR = 3,
} ospfv3_dr_candidate_class_t;

static const ospfv3_dr_candidate_t *ospfv3_best_dr_candidate(const GArray *candidates,
                                                             ospfv3_dr_candidate_class_t candidate_class,
                                                             uint32_t excluded_address)
{
    const ospfv3_dr_candidate_t *best = NULL;
    for (guint i = 0u; i < candidates->len; ++i)
    {
        const ospfv3_dr_candidate_t *candidate = &g_array_index(candidates, ospfv3_dr_candidate_t, i);
        if (candidate->priority == 0u || candidate->address == excluded_address)
        {
            continue;
        }
        if ((candidate_class == OSPFV3_DR_CANDIDATE_DECLARED_DR && !candidate->declared_dr) ||
            (candidate_class == OSPFV3_DR_CANDIDATE_DECLARED_BDR &&
             (candidate->declared_dr || !candidate->declared_bdr)) ||
            (candidate_class == OSPFV3_DR_CANDIDATE_NOT_DECLARED_DR && candidate->declared_dr))
        {
            continue;
        }
        if (!best || candidate->priority > best->priority ||
            (candidate->priority == best->priority && candidate->router_id > best->router_id))
        {
            best = candidate;
        }
    }
    return best;
}

static void ospfv3_elect_dr(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg)
{
    if (!inst || !cfg || cfg->network_type != OSPFV3_NETWORK_BROADCAST)
    {
        return;
    }
    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(cfg->ifname);
    if (!if_entry || if_entry->ipv6_linklocal_addr.family != AF_INET6)
    {
        return;
    }

    uint32_t local_address = inst->router_id;
    GArray *candidates = g_array_new(FALSE, FALSE, sizeof(ospfv3_dr_candidate_t));
    if (!candidates)
    {
        return;
    }
    ospfv3_dr_candidate_t local = {
        .router_id = inst->router_id,
        .address = local_address,
        .priority = cfg->priority,
        .declared_dr = cfg->dr == local_address,
        .declared_bdr = cfg->bdr == local_address,
    };
    g_array_append_val(candidates, local);

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_neighbor_t *nbr = (const ospfv3_neighbor_t *)value;
        if (nbr->state >= OSPFV3_NBR_STATE_TWO_WAY && nbr->area_id == cfg->area_id &&
            strcmp(nbr->ifname, cfg->ifname) == 0)
        {
            ospfv3_dr_candidate_t candidate = {
                .router_id = nbr->router_id,
                .address = nbr->router_id,
                .priority = nbr->priority,
                .declared_dr = nbr->dr == nbr->router_id,
                .declared_bdr = nbr->bdr == nbr->router_id,
            };
            g_array_append_val(candidates, candidate);
        }
    }

    const ospfv3_dr_candidate_t *bdr = ospfv3_best_dr_candidate(candidates, OSPFV3_DR_CANDIDATE_DECLARED_BDR, 0u);
    if (!bdr)
    {
        bdr = ospfv3_best_dr_candidate(candidates, OSPFV3_DR_CANDIDATE_NOT_DECLARED_DR, 0u);
    }
    const ospfv3_dr_candidate_t *dr = ospfv3_best_dr_candidate(candidates, OSPFV3_DR_CANDIDATE_DECLARED_DR, 0u);
    uint32_t elected_dr = dr ? dr->address : (bdr ? bdr->address : 0u);

    bdr = ospfv3_best_dr_candidate(candidates, OSPFV3_DR_CANDIDATE_DECLARED_BDR, elected_dr);
    if (!bdr)
    {
        bdr = ospfv3_best_dr_candidate(candidates, OSPFV3_DR_CANDIDATE_NOT_DECLARED_DR, elected_dr);
    }
    uint32_t elected_bdr = bdr ? bdr->address : 0u;
    g_array_free(candidates, TRUE);

    uint32_t old_dr = cfg->dr;
    uint32_t old_bdr = cfg->bdr;
    uint8_t old_state = cfg->state;
    cfg->dr = elected_dr;
    cfg->bdr = elected_bdr;
    cfg->wait_until_msec = 0u;
    cfg->state = local_address == cfg->dr    ? OSPFV3_IF_STATE_DR
                 : local_address == cfg->bdr ? OSPFV3_IF_STATE_BACKUP
                                             : OSPFV3_IF_STATE_DR_OTHER;

    gboolean should_join_dr = cfg->state == OSPFV3_IF_STATE_DR || cfg->state == OSPFV3_IF_STATE_BACKUP;
    if (should_join_dr && !cfg->joined_dr_routers && ospfv3_set_membership(cfg, &OSPFV3_ALL_DR_ROUTERS, 1) == 0)
    {
        cfg->joined_dr_routers = 1u;
    }
    else if (!should_join_dr && cfg->joined_dr_routers)
    {
        (void)ospfv3_set_membership(cfg, &OSPFV3_ALL_DR_ROUTERS, 0);
        cfg->joined_dr_routers = 0u;
    }

    gboolean topology_changed = old_dr != cfg->dr || old_bdr != cfg->bdr || old_state != cfg->state;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        ospfv3_neighbor_t *nbr = (ospfv3_neighbor_t *)value;
        if (nbr->state < OSPFV3_NBR_STATE_TWO_WAY || nbr->area_id != cfg->area_id ||
            strcmp(nbr->ifname, cfg->ifname) != 0)
        {
            continue;
        }
        if (ospfv3_adjacency_needed(inst, cfg, nbr))
        {
            ospfv3_neighbor_start_exchange(inst, cfg, nbr);
        }
        else if (nbr->state > OSPFV3_NBR_STATE_TWO_WAY)
        {
            nbr->state = OSPFV3_NBR_STATE_TWO_WAY;
            ospfv3_neighbor_clear_exchange(nbr);
            topology_changed = TRUE;
        }
    }
    if (topology_changed)
    {
        (void)ospfv3_send_hello(inst, cfg, ospfv3_now_msec());
        ospfv3_lsa_originate_all(inst, ospfv3_now_msec());
        ospfv3_spf_recalculate(inst);
    }
}

static void ospfv3_neighbor_mark_full(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr)
{
    if (nbr->state == OSPFV3_NBR_STATE_FULL)
    {
        return;
    }
    nbr->state = OSPFV3_NBR_STATE_FULL;
    char router_id[INET_ADDRSTRLEN];
    ospfv3_format_ipv4(nbr->router_id, router_id, sizeof(router_id));
    LOG_INFO("OSPFV3 process %u neighbor %s on %s reached Full", inst->process_id, router_id, nbr->ifname);
    ospfv3_lsa_originate_all(inst, ospfv3_now_msec());
    ospfv3_packet_send_lsdb(inst, nbr);
    ospfv3_spf_recalculate(inst);
    if (cfg->network_type == OSPFV3_NETWORK_BROADCAST)
    {
        ospfv3_elect_dr(inst, cfg);
    }
}

static void ospfv3_neighbor_exchange_done(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg, ospfv3_neighbor_t *nbr)
{
    if (nbr->request_keys && nbr->request_keys->len != 0u)
    {
        nbr->state = OSPFV3_NBR_STATE_LOADING;
        (void)ospfv3_send_lsr(inst, cfg, nbr);
        return;
    }
    ospfv3_neighbor_mark_full(inst, cfg, nbr);
}

static int ospfv3_request_matches(const ospfv3_lsa_request_t *request, uint16_t type, uint32_t link_state_id,
                                  uint32_t advertising_router)
{
    return request->type == type && request->link_state_id == link_state_id &&
           request->advertising_router == advertising_router;
}

static gboolean ospfv3_request_contains(const ospfv3_neighbor_t *nbr, uint16_t type, uint32_t link_state_id,
                                        uint32_t advertising_router)
{
    if (!nbr || !nbr->request_keys)
    {
        return FALSE;
    }
    for (guint i = 0u; i < nbr->request_keys->len; ++i)
    {
        const ospfv3_lsa_request_t *request = g_ptr_array_index(nbr->request_keys, i);
        if (ospfv3_request_matches(request, type, link_state_id, advertising_router))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void ospfv3_request_add(ospfv3_neighbor_t *nbr, const uint8_t *header)
{
    uint16_t type = ospfv3_get_u16(header + 2u);
    if (type != OSPFV3_LSA_ROUTER && type != OSPFV3_LSA_NETWORK && type != OSPFV3_LSA_LINK &&
        type != OSPFV3_LSA_INTRA_AREA_PREFIX)
    {
        return;
    }
    uint32_t link_state_id = ospfv3_get_u32(header + 4u);
    uint32_t advertising_router = ospfv3_get_u32(header + 8u);
    for (guint i = 0u; i < nbr->request_keys->len; ++i)
    {
        ospfv3_lsa_request_t *request = g_ptr_array_index(nbr->request_keys, i);
        if (ospfv3_request_matches(request, type, link_state_id, advertising_router))
        {
            return;
        }
    }
    ospfv3_lsa_request_t *request = g_malloc0(sizeof(*request));
    if (request)
    {
        request->type = type;
        request->link_state_id = link_state_id;
        request->advertising_router = advertising_router;
        g_ptr_array_add(nbr->request_keys, request);
    }
}

static void ospfv3_request_remove(ospfv3_neighbor_t *nbr, uint16_t type, uint32_t link_state_id,
                                  uint32_t advertising_router)
{
    if (!nbr->request_keys)
    {
        return;
    }
    for (guint i = nbr->request_keys->len; i > 0u; --i)
    {
        ospfv3_lsa_request_t *request = g_ptr_array_index(nbr->request_keys, i - 1u);
        if (ospfv3_request_matches(request, type, link_state_id, advertising_router))
        {
            g_ptr_array_remove_index(nbr->request_keys, i - 1u);
        }
    }
}

static void ospfv3_process_dbd_headers(ospfv3_instance_t *inst, ospfv3_neighbor_t *nbr, const uint8_t *headers,
                                       size_t headers_len)
{
    for (size_t offset = 0u; offset + OSPFV3_LSA_HEADER_LEN <= headers_len; offset += OSPFV3_LSA_HEADER_LEN)
    {
        const uint8_t *header = headers + offset;
        ospfv3_lsa_entry_t *current = ospfv3_lsa_lookup(inst, nbr->area_id, ospfv3_get_u16(header + 2u),
                                                        ospfv3_get_u32(header + 4u), ospfv3_get_u32(header + 8u));
        if (ospfv3_lsa_compare_header(header, current) > 0)
        {
            ospfv3_request_add(nbr, header);
        }
    }
}

static void ospfv3_handle_hello(ospfv3_packet_context_t *ctx, const struct in6_addr *source, uint32_t router_id,
                                const uint8_t *body, size_t body_len, uint64_t now_msec)
{
    if (body_len < 20u || ((body_len - 20u) % 4u) != 0u)
    {
        return;
    }
    uint32_t interface_id = ospfv3_get_u32(body);
    uint32_t options = ospfv3_get_u24(body + 5u);
    uint16_t hello_interval = ospfv3_get_u16(body + 8u);
    uint16_t dead_interval = ospfv3_get_u16(body + 10u);
    if (IN6_IS_ADDR_UNSPECIFIED(source) || !IN6_IS_ADDR_LINKLOCAL(source) ||
        memcmp(source, &ctx->if_entry->ipv6_linklocal_addr.u.v6, sizeof(*source)) == 0 || interface_id == 0u ||
        hello_interval != ctx->cfg->hello_interval || dead_interval != ctx->cfg->dead_interval ||
        (options & OSPFV3_OPTIONS_V6) == 0u)
    {
        return;
    }

    ospfv3_neighbor_t *nbr = ospfv3_neighbor_get_or_create(ctx->inst, ctx->cfg, router_id);
    if (!nbr)
    {
        return;
    }
    if (!IN6_IS_ADDR_UNSPECIFIED(&nbr->src_addr) && memcmp(&nbr->src_addr, source, sizeof(*source)) != 0)
    {
        return;
    }
    nbr->src_addr = *source;
    nbr->interface_id = interface_id;
    nbr->priority = body[4];
    nbr->options = options;
    nbr->dead_interval = dead_interval;
    nbr->dr = ospfv3_get_u32(body + 12u);
    nbr->bdr = ospfv3_get_u32(body + 16u);
    nbr->last_seen_msec = now_msec;

    gboolean seen_self = FALSE;
    for (size_t offset = 20u; offset + 4u <= body_len; offset += 4u)
    {
        if (ospfv3_get_u32(body + offset) == ctx->inst->router_id)
        {
            seen_self = TRUE;
            break;
        }
    }

    if (!seen_self)
    {
        gboolean topology_changed = nbr->state >= OSPFV3_NBR_STATE_TWO_WAY;
        nbr->state = OSPFV3_NBR_STATE_INIT;
        ospfv3_neighbor_clear_exchange(nbr);
        if (topology_changed)
        {
            if (ctx->cfg->network_type == OSPFV3_NETWORK_BROADCAST && ctx->cfg->state != OSPFV3_IF_STATE_WAITING)
            {
                ospfv3_elect_dr(ctx->inst, ctx->cfg);
            }
            ospfv3_lsa_originate_all(ctx->inst, now_msec);
            ospfv3_spf_recalculate(ctx->inst);
        }
        return;
    }
    if (nbr->state < OSPFV3_NBR_STATE_TWO_WAY && ospfv3_neighbor_has_parallel_adjacency(ctx->inst, nbr))
    {
        return;
    }
    if (nbr->state < OSPFV3_NBR_STATE_TWO_WAY)
    {
        nbr->state = OSPFV3_NBR_STATE_TWO_WAY;
    }

    if (ctx->cfg->network_type == OSPFV3_NETWORK_BROADCAST)
    {
        gboolean backup_seen = nbr->bdr == nbr->router_id || (nbr->dr == nbr->router_id && nbr->bdr == 0u);
        if (ctx->cfg->state != OSPFV3_IF_STATE_WAITING || backup_seen)
        {
            ospfv3_elect_dr(ctx->inst, ctx->cfg);
        }
    }
    else if (ospfv3_adjacency_needed(ctx->inst, ctx->cfg, nbr))
    {
        ospfv3_neighbor_start_exchange(ctx->inst, ctx->cfg, nbr);
    }
}

static void ospfv3_handle_dbd(ospfv3_packet_context_t *ctx, uint32_t router_id, const uint8_t *body, size_t body_len)
{
    ospfv3_neighbor_t *nbr = ospfv3_neighbor_lookup(ctx->inst, ctx->cfg->ifname, router_id);
    if (!nbr || nbr->state < OSPFV3_NBR_STATE_TWO_WAY || !ospfv3_adjacency_needed(ctx->inst, ctx->cfg, nbr))
    {
        return;
    }
    if (body_len < 12u || ((body_len - 12u) % OSPFV3_LSA_HEADER_LEN) != 0u)
    {
        if (nbr->state >= OSPFV3_NBR_STATE_EXSTART)
        {
            ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
        }
        return;
    }

    uint16_t peer_mtu = ospfv3_get_u16(body + 4u);
    uint16_t local_mtu = ospfv3_interface_mtu(ctx->cfg);
    uint32_t options = ospfv3_get_u24(body + 1u);
    uint8_t flags = body[7];
    uint8_t valid_flags = OSPFV3_DBD_FLAG_I | OSPFV3_DBD_FLAG_M | OSPFV3_DBD_FLAG_MS;
    if ((options & OSPFV3_OPTIONS_V6) == 0u || (flags & (uint8_t)~valid_flags) != 0u || peer_mtu == 0u ||
        peer_mtu > local_mtu)
    {
        if (nbr->state >= OSPFV3_NBR_STATE_EXSTART)
        {
            ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
        }
        return;
    }

    if (nbr->state == OSPFV3_NBR_STATE_TWO_WAY)
    {
        ospfv3_neighbor_start_exchange(ctx->inst, ctx->cfg, nbr);
    }

    uint32_t sequence = ospfv3_get_u32(body + 8u);
    gboolean initial = (flags & (OSPFV3_DBD_FLAG_I | OSPFV3_DBD_FLAG_M | OSPFV3_DBD_FLAG_MS)) ==
                           (OSPFV3_DBD_FLAG_I | OSPFV3_DBD_FLAG_M | OSPFV3_DBD_FLAG_MS) &&
                       body_len == 12u;
    gboolean duplicate = ospfv3_received_dbd_is_duplicate(nbr, body, body_len);

    if (nbr->state >= OSPFV3_NBR_STATE_LOADING)
    {
        if (duplicate)
        {
            if (!nbr->dd_master)
            {
                (void)ospfv3_retransmit_last_dbd(ctx->cfg, nbr);
            }
            return;
        }
        ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
        return;
    }

    if (nbr->state == OSPFV3_NBR_STATE_EXCHANGE && duplicate)
    {
        if (!nbr->dd_master)
        {
            (void)ospfv3_retransmit_last_dbd(ctx->cfg, nbr);
        }
        return;
    }

    if (nbr->state == OSPFV3_NBR_STATE_EXSTART)
    {
        if (initial)
        {
            if (router_id > ctx->inst->router_id)
            {
                nbr->dd_master = 0u;
                nbr->dd_sequence = sequence;
                nbr->dd_peer_mtu = peer_mtu;
                nbr->dd_options = options;
                nbr->state = OSPFV3_NBR_STATE_EXCHANGE;
                if (!ospfv3_cache_received_dbd(nbr, body, body_len))
                {
                    ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
                    return;
                }
                (void)ospfv3_send_dbd(ctx->inst, ctx->cfg, nbr, 0u, 1);
            }
            else
            {
                nbr->dd_master = 1u;
                (void)ospfv3_retransmit_last_dbd(ctx->cfg, nbr);
            }
            return;
        }

        if (ctx->inst->router_id > router_id && (flags & (OSPFV3_DBD_FLAG_I | OSPFV3_DBD_FLAG_MS)) == 0u &&
            sequence == nbr->dd_sequence)
        {
            nbr->dd_master = 1u;
            nbr->dd_peer_mtu = peer_mtu;
            nbr->dd_options = options;
            nbr->state = OSPFV3_NBR_STATE_EXCHANGE;
            nbr->dd_peer_more = (flags & OSPFV3_DBD_FLAG_M) != 0u;
            if (!ospfv3_cache_received_dbd(nbr, body, body_len))
            {
                ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
                return;
            }
            ospfv3_process_dbd_headers(ctx->inst, nbr, body + 12u, body_len - 12u);
            nbr->dd_sequence++;
            (void)ospfv3_send_dbd(ctx->inst, ctx->cfg, nbr, OSPFV3_DBD_FLAG_MS, 1);
            return;
        }
        ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
        return;
    }

    if (nbr->state != OSPFV3_NBR_STATE_EXCHANGE)
    {
        return;
    }
    if (options != nbr->dd_options)
    {
        ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
        return;
    }

    if (nbr->dd_master)
    {
        if ((flags & (OSPFV3_DBD_FLAG_I | OSPFV3_DBD_FLAG_MS)) != 0u || sequence != nbr->dd_sequence)
        {
            ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
            return;
        }
        nbr->dd_peer_mtu = peer_mtu;
        nbr->dd_peer_more = (flags & OSPFV3_DBD_FLAG_M) != 0u;
        if (!ospfv3_cache_received_dbd(nbr, body, body_len))
        {
            ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
            return;
        }
        ospfv3_process_dbd_headers(ctx->inst, nbr, body + 12u, body_len - 12u);
        if (!nbr->dd_peer_more && !nbr->dd_local_more)
        {
            ospfv3_neighbor_exchange_done(ctx->inst, ctx->cfg, nbr);
        }
        else
        {
            nbr->dd_sequence++;
            (void)ospfv3_send_dbd(ctx->inst, ctx->cfg, nbr, OSPFV3_DBD_FLAG_MS, 1);
        }
        return;
    }

    if ((flags & (OSPFV3_DBD_FLAG_I | OSPFV3_DBD_FLAG_MS)) != OSPFV3_DBD_FLAG_MS)
    {
        ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
        return;
    }
    if (sequence != nbr->dd_sequence + 1u)
    {
        ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
        return;
    }

    nbr->dd_sequence = sequence;
    nbr->dd_peer_mtu = peer_mtu;
    nbr->dd_peer_more = (flags & OSPFV3_DBD_FLAG_M) != 0u;
    if (!ospfv3_cache_received_dbd(nbr, body, body_len))
    {
        ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
        return;
    }
    ospfv3_process_dbd_headers(ctx->inst, nbr, body + 12u, body_len - 12u);
    (void)ospfv3_send_dbd(ctx->inst, ctx->cfg, nbr, 0u, 1);
    if (!nbr->dd_peer_more && !nbr->dd_local_more)
    {
        ospfv3_neighbor_exchange_done(ctx->inst, ctx->cfg, nbr);
    }
}

static void ospfv3_handle_lsr(ospfv3_packet_context_t *ctx, uint32_t router_id, const uint8_t *body, size_t body_len)
{
    if (body_len == 0u || (body_len % 12u) != 0u)
    {
        return;
    }
    ospfv3_neighbor_t *nbr = ospfv3_neighbor_lookup(ctx->inst, ctx->cfg->ifname, router_id);
    if (!nbr || nbr->state < OSPFV3_NBR_STATE_EXCHANGE)
    {
        return;
    }

    for (size_t offset = 0u; offset + 12u <= body_len; offset += 12u)
    {
        uint32_t type = ospfv3_get_u32(body + offset);
        if (type > UINT16_MAX)
        {
            ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
            return;
        }
        ospfv3_lsa_entry_t *entry =
            ospfv3_lsa_lookup(ctx->inst, ctx->cfg->area_id, (uint16_t)type, ospfv3_get_u32(body + offset + 4u),
                              ospfv3_get_u32(body + offset + 8u));
        if (!entry || !entry->raw)
        {
            ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
            return;
        }
    }

    for (size_t offset = 0u; offset + 12u <= body_len; offset += 12u)
    {
        ospfv3_lsa_entry_t *entry =
            ospfv3_lsa_lookup(ctx->inst, ctx->cfg->area_id, (uint16_t)ospfv3_get_u32(body + offset),
                              ospfv3_get_u32(body + offset + 4u), ospfv3_get_u32(body + offset + 8u));
        (void)ospfv3_send_lsa_entry(ctx->inst, ctx->cfg, nbr, entry, TRUE);
    }
}

static void ospfv3_handle_lsu(ospfv3_packet_context_t *ctx, uint32_t router_id, const uint8_t *body, size_t body_len,
                              uint64_t now_msec)
{
    if (body_len < 4u)
    {
        return;
    }
    ospfv3_neighbor_t *nbr = ospfv3_neighbor_lookup(ctx->inst, ctx->cfg->ifname, router_id);
    if (!nbr || nbr->state < OSPFV3_NBR_STATE_EXCHANGE)
    {
        return;
    }

    uint32_t count = ospfv3_get_u32(body);
    if ((size_t)count > (body_len - 4u) / OSPFV3_LSA_HEADER_LEN)
    {
        return;
    }

    size_t validated_offset = 4u;
    for (uint32_t i = 0u; i < count; ++i)
    {
        if (body_len - validated_offset < OSPFV3_LSA_HEADER_LEN)
        {
            return;
        }
        uint16_t length = ospfv3_get_u16(body + validated_offset + 18u);
        if (length < OSPFV3_LSA_HEADER_LEN || length > body_len - validated_offset)
        {
            return;
        }
        validated_offset += length;
    }
    if (validated_offset != body_len)
    {
        return;
    }

    size_t offset = 4u;
    gboolean changed_any = FALSE;
    gboolean fight_back = FALSE;
    GByteArray *acks = g_byte_array_sized_new((size_t)count * OSPFV3_LSA_HEADER_LEN);
    if (!acks)
    {
        return;
    }
    for (uint32_t i = 0u; i < count; ++i)
    {
        const uint8_t *lsa = body + offset;
        uint16_t length = ospfv3_get_u16(lsa + 18u);

        int changed = 0;
        int comparison = 0;
        uint16_t lsa_type = ospfv3_get_u16(lsa + 2u);
        if ((lsa_type == OSPFV3_LSA_ROUTER || lsa_type == OSPFV3_LSA_NETWORK || lsa_type == OSPFV3_LSA_LINK ||
             lsa_type == OSPFV3_LSA_INTRA_AREA_PREFIX) &&
            ospfv3_lsa_install(ctx->inst, ctx->cfg->area_id, lsa, length, now_msec, 0, &changed, &comparison) == 0)
        {
            uint32_t link_state_id = ospfv3_get_u32(lsa + 4u);
            uint32_t advertising_router = ospfv3_get_u32(lsa + 8u);
            if (comparison < 0)
            {
                if (ospfv3_request_contains(nbr, lsa_type, link_state_id, advertising_router))
                {
                    g_byte_array_unref(acks);
                    ospfv3_neighbor_restart_exchange(ctx->inst, ctx->cfg, nbr);
                    return;
                }
                ospfv3_lsa_entry_t *current =
                    ospfv3_lsa_lookup(ctx->inst, ctx->cfg->area_id, lsa_type, link_state_id, advertising_router);
                if (current)
                {
                    (void)ospfv3_send_lsa_entry(ctx->inst, ctx->cfg, nbr, current, FALSE);
                }
                offset += length;
                continue;
            }

            ospfv3_retransmit_acknowledge(nbr, lsa, TRUE);
            g_byte_array_append(acks, lsa, OSPFV3_LSA_HEADER_LEN);
            ospfv3_request_remove(nbr, lsa_type, link_state_id, advertising_router);
            if (changed)
            {
                changed_any = TRUE;
                if (advertising_router == ctx->inst->router_id)
                {
                    fight_back = TRUE;
                }
                else
                {
                    ospfv3_lsa_entry_t *installed =
                        ospfv3_lsa_lookup(ctx->inst, ctx->cfg->area_id, lsa_type, link_state_id, advertising_router);
                    if (installed)
                    {
                        ospfv3_packet_flood_lsa(ctx->inst, installed, nbr);
                    }
                }
            }
        }
        offset += length;
    }

    (void)ospfv3_send_ack(ctx->inst, ctx->cfg, nbr, acks);
    g_byte_array_unref(acks);
    if (fight_back)
    {
        ospfv3_lsa_originate_all(ctx->inst, now_msec);
    }
    if (changed_any)
    {
        ospfv3_spf_recalculate(ctx->inst);
    }
    if (nbr->state == OSPFV3_NBR_STATE_LOADING && nbr->request_keys->len == 0u)
    {
        ospfv3_neighbor_mark_full(ctx->inst, ctx->cfg, nbr);
    }
}

static void ospfv3_handle_ls_ack(ospfv3_packet_context_t *ctx, uint32_t router_id, const uint8_t *body, size_t body_len)
{
    if (body_len == 0u || (body_len % OSPFV3_LSA_HEADER_LEN) != 0u)
    {
        return;
    }
    ospfv3_neighbor_t *nbr = ospfv3_neighbor_lookup(ctx->inst, ctx->cfg->ifname, router_id);
    if (!nbr || nbr->state < OSPFV3_NBR_STATE_EXCHANGE)
    {
        return;
    }

    for (size_t offset = 0u; offset < body_len; offset += OSPFV3_LSA_HEADER_LEN)
    {
        ospfv3_retransmit_acknowledge(nbr, body + offset, FALSE);
    }
}

static void ospfv3_handle_packet(uint32_t ifindex, const struct in6_addr *source, const struct in6_addr *destination,
                                 const uint8_t *packet, size_t packet_len)
{
    if (packet_len < OSPFV3_HEADER_LEN || packet[0] != OSPFV3_VERSION || packet[1] < OSPFV3_PACKET_HELLO ||
        packet[1] > OSPFV3_PACKET_LS_ACK || ospfv3_get_u16(packet + 2u) != packet_len ||
        packet[14] != OSPFV3_INSTANCE_ID || packet[15] != 0u ||
        ospfv3_packet_checksum(source, destination, packet, packet_len) != 0u)
    {
        return;
    }

    uint32_t router_id = ospfv3_get_u32(packet + 4u);
    uint32_t area_id = ospfv3_get_u32(packet + 8u);
    if (router_id == 0u)
    {
        return;
    }

    ospfv3_packet_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    GHashTableIter inst_iter;
    gpointer inst_value = NULL;
    g_hash_table_iter_init(&inst_iter, g_ospfv3_work_local->instances);
    while (g_hash_table_iter_next(&inst_iter, NULL, &inst_value) && !ctx.inst)
    {
        ospfv3_instance_t *inst = (ospfv3_instance_t *)inst_value;
        if (inst->router_id == router_id)
        {
            continue;
        }
        GHashTableIter if_iter;
        gpointer if_value = NULL;
        g_hash_table_iter_init(&if_iter, inst->if_cfgs);
        while (g_hash_table_iter_next(&if_iter, NULL, &if_value))
        {
            ospfv3_if_cfg_t *cfg = (ospfv3_if_cfg_t *)if_value;
            const if_api_cache_entry_t *if_entry = NULL;
            if (cfg->area_id == area_id && ospfv3_interface_ready(cfg, &if_entry) && if_entry->ifindex == ifindex)
            {
                ctx.inst = inst;
                ctx.cfg = cfg;
                ctx.if_entry = if_entry;
                break;
            }
        }
    }
    if (!ctx.inst)
    {
        return;
    }
    if (packet[1] != OSPFV3_PACKET_HELLO)
    {
        const ospfv3_neighbor_t *nbr = ospfv3_neighbor_lookup(ctx.inst, ctx.cfg->ifname, router_id);
        if (!nbr || memcmp(&nbr->src_addr, source, sizeof(*source)) != 0)
        {
            return;
        }
    }

    const uint8_t *body = packet + OSPFV3_HEADER_LEN;
    size_t body_len = packet_len - OSPFV3_HEADER_LEN;
    uint64_t now_msec = ospfv3_now_msec();
    switch (packet[1])
    {
        case OSPFV3_PACKET_HELLO:
            ospfv3_handle_hello(&ctx, source, router_id, body, body_len, now_msec);
            break;
        case OSPFV3_PACKET_DBD:
            ospfv3_handle_dbd(&ctx, router_id, body, body_len);
            break;
        case OSPFV3_PACKET_LS_REQUEST:
            ospfv3_handle_lsr(&ctx, router_id, body, body_len);
            break;
        case OSPFV3_PACKET_LS_UPDATE:
            ospfv3_handle_lsu(&ctx, router_id, body, body_len, now_msec);
            break;
        case OSPFV3_PACKET_LS_ACK:
            ospfv3_handle_ls_ack(&ctx, router_id, body, body_len);
            break;
        default:
            break;
    }
}

int ospfv3_packet_socket_open(void)
{
    int fd = socket(AF_INET6, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, OSPFV3_IP_PROTOCOL);
    if (fd < 0)
    {
        LOG_PERROR("OSPFV3: raw socket open failed");
        return -1;
    }

    int enabled = 1;
    int hops = 1;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &enabled, sizeof(enabled)) < 0)
    {
        LOG_PERROR("OSPFV3: raw socket setup failed");
        close(fd);
        return -1;
    }
    return fd;
}

void ospfv3_packet_socket_close(void)
{
    if (g_ospfv3_work_local && g_ospfv3_work_local->raw_fd >= 0)
    {
        close(g_ospfv3_work_local->raw_fd);
        g_ospfv3_work_local->raw_fd = -1;
    }
}

void ospfv3_packet_handle_read(void)
{
    if (!g_ospfv3_work_local || g_ospfv3_work_local->raw_fd < 0)
    {
        return;
    }

    for (;;)
    {
        uint8_t buffer[OSPFV3_PACKET_BUFFER_SIZE];
        uint8_t control[CMSG_SPACE(sizeof(struct in6_pktinfo)) + CMSG_SPACE(sizeof(int))];
        struct sockaddr_in6 source_addr;
        struct iovec iov = {.iov_base = buffer, .iov_len = sizeof(buffer)};
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        memset(&source_addr, 0, sizeof(source_addr));
        msg.msg_name = &source_addr;
        msg.msg_namelen = sizeof(source_addr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1u;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        ssize_t received = recvmsg(g_ospfv3_work_local->raw_fd, &msg, MSG_DONTWAIT);
        if (received < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            {
                LOG_WARN("OSPFV3: packet receive failed: %s", g_strerror(errno));
            }
            return;
        }
        if (received == 0 || (msg.msg_flags & MSG_TRUNC) != 0)
        {
            continue;
        }

        uint32_t ifindex = 0u;
        int hop_limit = -1;
        struct in6_addr destination = IN6ADDR_ANY_INIT;
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo)))
            {
                const struct in6_pktinfo *pktinfo = (const struct in6_pktinfo *)CMSG_DATA(cmsg);
                ifindex = pktinfo->ipi6_ifindex;
                destination = pktinfo->ipi6_addr;
            }
            else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_HOPLIMIT &&
                     cmsg->cmsg_len >= CMSG_LEN(sizeof(int)))
            {
                memcpy(&hop_limit, CMSG_DATA(cmsg), sizeof(hop_limit));
            }
        }
        if (ifindex == 0u || hop_limit != 1 || !IN6_IS_ADDR_LINKLOCAL(&source_addr.sin6_addr))
        {
            continue;
        }

        size_t ospfv3_len = (size_t)received;
        uint16_t declared_len = ospfv3_len >= 4u ? ospfv3_get_u16(buffer + 2u) : 0u;
        if (declared_len < OSPFV3_HEADER_LEN || declared_len > ospfv3_len)
        {
            continue;
        }
        ospfv3_handle_packet(ifindex, &source_addr.sin6_addr, &destination, buffer, declared_len);
    }
}

static void ospfv3_drop_memberships(ospfv3_if_cfg_t *cfg)
{
    if (cfg->joined_dr_routers)
    {
        (void)ospfv3_set_membership(cfg, &OSPFV3_ALL_DR_ROUTERS, 0);
        cfg->joined_dr_routers = 0u;
    }
    if (cfg->joined_all_routers)
    {
        (void)ospfv3_set_membership(cfg, &OSPFV3_ALL_SPF_ROUTERS, 0);
        cfg->joined_all_routers = 0u;
    }
}

static gboolean ospfv3_remove_neighbors_on_interface(ospfv3_instance_t *inst, const char *ifname)
{
    gboolean removed = FALSE;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospfv3_neighbor_t *nbr = (const ospfv3_neighbor_t *)value;
        if (strcmp(nbr->ifname, ifname) == 0)
        {
            g_hash_table_iter_remove(&iter);
            removed = TRUE;
        }
    }
    return removed;
}

void ospfv3_packet_reconcile_interface(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg)
{
    const if_api_cache_entry_t *if_entry = NULL;
    if (!inst || !cfg)
    {
        return;
    }
    if (!ospfv3_interface_ready(cfg, &if_entry))
    {
        ospfv3_packet_remove_interface(inst, cfg->ifname);
        return;
    }

    if (!cfg->joined_all_routers && ospfv3_set_membership(cfg, &OSPFV3_ALL_SPF_ROUTERS, 1) == 0)
    {
        cfg->joined_all_routers = 1u;
    }
    if (cfg->network_type == OSPFV3_NETWORK_POINT_TO_POINT)
    {
        if (cfg->joined_dr_routers)
        {
            (void)ospfv3_set_membership(cfg, &OSPFV3_ALL_DR_ROUTERS, 0);
            cfg->joined_dr_routers = 0u;
        }
        cfg->state = OSPFV3_IF_STATE_POINT_TO_POINT;
        cfg->dr = 0u;
        cfg->bdr = 0u;
        cfg->wait_until_msec = 0u;
    }
    else
    {
        if (cfg->state == OSPFV3_IF_STATE_DOWN)
        {
            cfg->state = OSPFV3_IF_STATE_WAITING;
            cfg->dr = 0u;
            cfg->bdr = 0u;
            cfg->wait_until_msec = ospfv3_now_msec() + ((uint64_t)cfg->dead_interval * 1000u);
        }
    }

    if (cfg->last_hello_tx_msec == 0u)
    {
        (void)ospfv3_send_hello(inst, cfg, ospfv3_now_msec());
    }
}

void ospfv3_packet_remove_interface(ospfv3_instance_t *inst, const char *ifname)
{
    if (!inst || !ifname)
    {
        return;
    }
    ospfv3_if_cfg_t *cfg = g_hash_table_lookup(inst->if_cfgs, ifname);
    if (cfg)
    {
        ospfv3_drop_memberships(cfg);
        cfg->state = OSPFV3_IF_STATE_DOWN;
        cfg->dr = 0u;
        cfg->bdr = 0u;
        cfg->last_hello_tx_msec = 0u;
        cfg->wait_until_msec = 0u;
    }
    (void)ospfv3_remove_neighbors_on_interface(inst, ifname);
}

void ospfv3_packet_reset_instance(ospfv3_instance_t *inst)
{
    if (!inst)
    {
        return;
    }
    ospfv3_lsa_flush_self(inst);
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->if_cfgs);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        ospfv3_if_cfg_t *cfg = (ospfv3_if_cfg_t *)value;
        ospfv3_drop_memberships(cfg);
        cfg->state = OSPFV3_IF_STATE_DOWN;
        cfg->dr = 0u;
        cfg->bdr = 0u;
        cfg->last_hello_tx_msec = 0u;
        cfg->wait_until_msec = 0u;
    }
    g_hash_table_remove_all(inst->neighbors);
    inst->next_lsa_originate_msec = 0u;
}

void ospfv3_packet_tick(uint64_t now_msec)
{
    if (!g_ospfv3_work_local || !g_ospfv3_work_local->instances)
    {
        return;
    }

    GHashTableIter inst_iter;
    gpointer inst_value = NULL;
    g_hash_table_iter_init(&inst_iter, g_ospfv3_work_local->instances);
    while (g_hash_table_iter_next(&inst_iter, NULL, &inst_value))
    {
        ospfv3_instance_t *inst = (ospfv3_instance_t *)inst_value;
        GHashTableIter if_iter;
        gpointer if_value = NULL;
        g_hash_table_iter_init(&if_iter, inst->if_cfgs);
        while (g_hash_table_iter_next(&if_iter, NULL, &if_value))
        {
            ospfv3_if_cfg_t *cfg = (ospfv3_if_cfg_t *)if_value;
            if (!ospfv3_interface_ready(cfg, NULL))
            {
                continue;
            }
            if (cfg->network_type == OSPFV3_NETWORK_BROADCAST && cfg->state == OSPFV3_IF_STATE_WAITING &&
                cfg->wait_until_msec != 0u && now_msec >= cfg->wait_until_msec)
            {
                ospfv3_elect_dr(inst, cfg);
            }
            uint64_t hello_interval = (uint64_t)cfg->hello_interval * 1000u;
            if (cfg->last_hello_tx_msec == 0u || now_msec - cfg->last_hello_tx_msec >= hello_interval)
            {
                (void)ospfv3_send_hello(inst, cfg, now_msec);
            }
        }

        gboolean topology_changed = FALSE;
        GHashTableIter nbr_iter;
        gpointer nbr_value = NULL;
        g_hash_table_iter_init(&nbr_iter, inst->neighbors);
        while (g_hash_table_iter_next(&nbr_iter, NULL, &nbr_value))
        {
            ospfv3_neighbor_t *nbr = (ospfv3_neighbor_t *)nbr_value;
            ospfv3_if_cfg_t *cfg = g_hash_table_lookup(inst->if_cfgs, nbr->ifname);
            uint64_t dead_msec =
                (uint64_t)(nbr->dead_interval ? nbr->dead_interval : (cfg ? cfg->dead_interval : 1u)) * 1000u;
            if (!cfg || now_msec - nbr->last_seen_msec >= dead_msec)
            {
                char router_id[INET_ADDRSTRLEN];
                ospfv3_format_ipv4(nbr->router_id, router_id, sizeof(router_id));
                LOG_INFO("OSPFV3 process %u neighbor %s on %s expired", inst->process_id, router_id, nbr->ifname);
                topology_changed = topology_changed || nbr->state >= OSPFV3_NBR_STATE_TWO_WAY;
                g_hash_table_iter_remove(&nbr_iter);
                continue;
            }
            if ((nbr->state == OSPFV3_NBR_STATE_EXSTART || nbr->state == OSPFV3_NBR_STATE_EXCHANGE) && nbr->dd_master &&
                now_msec - nbr->last_dbd_tx_msec >= OSPFV3_RETRANSMIT_INTERVAL_MSEC)
            {
                (void)ospfv3_retransmit_last_dbd(cfg, nbr);
            }
            else if (nbr->state == OSPFV3_NBR_STATE_LOADING &&
                     now_msec - nbr->last_dbd_tx_msec >= OSPFV3_RETRANSMIT_INTERVAL_MSEC)
            {
                (void)ospfv3_send_lsr(inst, cfg, nbr);
            }
            ospfv3_retransmit_pending(inst, cfg, nbr, now_msec);
        }
        if (topology_changed)
        {
            g_hash_table_iter_init(&if_iter, inst->if_cfgs);
            while (g_hash_table_iter_next(&if_iter, NULL, &if_value))
            {
                ospfv3_if_cfg_t *cfg = (ospfv3_if_cfg_t *)if_value;
                if (cfg->network_type == OSPFV3_NETWORK_BROADCAST && cfg->state != OSPFV3_IF_STATE_WAITING)
                {
                    ospfv3_elect_dr(inst, cfg);
                }
            }
            ospfv3_lsa_originate_all(inst, now_msec);
            ospfv3_spf_recalculate(inst);
        }
    }
}
