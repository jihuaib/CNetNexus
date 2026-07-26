/**
 * @file   lldp_worker.c
 * @brief  LLDP worker：epoll + raw socket + timer
 * @author jhb
 * @date   2026/06/07
 */
#include "lldp_worker.h"

#include <ctype.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "errcode.h"
#include "if.h"
#include "lldp.h"
#include "lldp_packet.h"
#include "lldp_show.h"
#include "lldp_snmp_report.h"
#include "log.h"
#include "syslog_report.h"

#define LLDP_MAX_EPOLL_EVENTS 8
#define LLDP_MAX_FRAME_LEN 1500u
#define LLDP_EVT_CMD 1u
#define LLDP_EVT_TIMER 2u
#define LLDP_EVT_RAW 3u

static const uint8_t g_lldp_dst_mac[ETH_ALEN] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e};

lldp_work_local_t *g_lldp_work_local = NULL;

void lldp_worker_lock(void)
{
    if (g_lldp_work_local)
    {
        pthread_mutex_lock(&g_lldp_work_local->lock);
    }
}

void lldp_worker_unlock(void)
{
    if (g_lldp_work_local)
    {
        pthread_mutex_unlock(&g_lldp_work_local->lock);
    }
}

static uint64_t lldp_now_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static uint32_t effective_tx_interval(const lldp_iface_state_t *iface)
{
    if (iface && iface->tx_interval_sec != 0u)
    {
        return iface->tx_interval_sec;
    }
    if (g_lldp_work_local && g_lldp_work_local->proto.tx_interval_sec != 0u)
    {
        return g_lldp_work_local->proto.tx_interval_sec;
    }
    return LLDP_DEFAULT_TX_INTERVAL_SEC;
}

static uint32_t effective_hold_multiplier(const lldp_iface_state_t *iface)
{
    if (iface && iface->hold_multiplier != 0u)
    {
        return iface->hold_multiplier;
    }
    if (g_lldp_work_local && g_lldp_work_local->proto.hold_multiplier != 0u)
    {
        return g_lldp_work_local->proto.hold_multiplier;
    }
    return LLDP_DEFAULT_HOLD_MULTIPLIER;
}

static int iface_tx_enabled(const lldp_iface_state_t *iface)
{
    return iface && iface->enabled && iface->link_up && iface->ifindex != 0u &&
           (iface->admin_status == LLDP_IF_ADMIN_TX_RX || iface->admin_status == LLDP_IF_ADMIN_TX_ONLY);
}

static int iface_rx_enabled(const lldp_iface_state_t *iface)
{
    return iface && iface->enabled &&
           (iface->admin_status == LLDP_IF_ADMIN_TX_RX || iface->admin_status == LLDP_IF_ADMIN_RX_ONLY);
}

static int cache_entry_lldp_eligible(const if_api_cache_entry_t *entry)
{
    return entry && entry->logical_name[0] && entry->physical_name[0] && entry->ifindex != 0u &&
           strcmp(entry->logical_name, "null0") != 0 && strncmp(entry->logical_name, "loop", 4) != 0;
}

static lldp_iface_state_t *ensure_default_iface_from_cache(const if_api_cache_entry_t *entry)
{
    if (!g_lldp_work_local || !g_lldp_work_local->interfaces || !cache_entry_lldp_eligible(entry))
    {
        return NULL;
    }

    lldp_iface_state_t *iface = g_hash_table_lookup(g_lldp_work_local->interfaces, entry->logical_name);
    if (!iface)
    {
        iface = g_malloc0(sizeof(*iface));
        if (!iface)
        {
            return NULL;
        }
        g_strlcpy(iface->ifname, entry->logical_name, sizeof(iface->ifname));
        iface->enabled = 1u;
        iface->admin_status = LLDP_IF_ADMIN_TX_RX;
        g_hash_table_insert(g_lldp_work_local->interfaces, g_strdup(iface->ifname), iface);
    }

    iface->ifindex = entry->ifindex;
    iface->link_up = entry->link_up ? 1u : 0u;
    return iface;
}

static gboolean ensure_default_iface_cb(const if_api_cache_entry_t *entry, void *user_data)
{
    (void)user_data;
    (void)ensure_default_iface_from_cache(entry);
    return FALSE;
}

static void iface_free(gpointer data)
{
    g_free(data);
}

static void close_fd(int *fd)
{
    if (fd && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

static int get_src_mac(int fd, const if_api_cache_entry_t *if_entry, uint8_t out_mac[ETH_ALEN])
{
    if (!if_entry || !out_mac)
    {
        return -1;
    }

    char os_ifname[IFNAMSIZ] = {0};
    if (if_entry->physical_name[0])
    {
        g_strlcpy(os_ifname, if_entry->physical_name, sizeof(os_ifname));
    }
    else if (if_entry->ifindex != 0u)
    {
        (void)if_indextoname(if_entry->ifindex, os_ifname);
    }
    if (!os_ifname[0])
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

static int build_lldpdu(const lldp_iface_state_t *iface, const uint8_t src_mac[ETH_ALEN], uint16_t ttl, uint8_t *buf,
                        size_t cap, size_t *len_out)
{
    lldp_packet_build_info_t info;
    memset(&info, 0, sizeof(info));

    info.chassis_id.subtype = LLDP_CHASSIS_SUBTYPE_MAC_ADDRESS;
    info.chassis_id.len = ETH_ALEN;
    memcpy(info.chassis_id.data, src_mac, ETH_ALEN);

    info.port_id.subtype = LLDP_PORT_SUBTYPE_INTERFACE_NAME;
    info.port_id.len = (uint16_t)strnlen(iface->ifname, sizeof(iface->ifname));
    memcpy(info.port_id.data, iface->ifname, info.port_id.len);

    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1u) != 0 || hostname[0] == '\0')
    {
        g_strlcpy(hostname, "netnexus", sizeof(hostname));
    }

    info.ttl = ttl;
    info.port_desc = iface->port_desc[0] ? iface->port_desc : iface->ifname;
    info.system_name = hostname;
    info.system_desc = "CNetNexus LLDP";
    info.caps_supported = 0x0010u;
    info.caps_enabled = 0x0010u;
    return lldp_packet_build_basic(buf, cap, &info, len_out);
}

static void send_on_iface(lldp_iface_state_t *iface, uint16_t ttl)
{
    if (!g_lldp_work_local || g_lldp_work_local->raw_fd < 0 || !iface_tx_enabled(iface))
    {
        return;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(iface->ifname);
    if (!if_entry || if_entry->ifindex == 0u || !if_entry->link_up)
    {
        iface->ifindex = 0u;
        iface->link_up = 0u;
        return;
    }
    iface->ifindex = if_entry->ifindex;
    iface->link_up = 1u;

    uint8_t src_mac[ETH_ALEN];
    if (get_src_mac(g_lldp_work_local->raw_fd, if_entry, src_mac) != 0)
    {
        g_lldp_work_local->stats.tx_errors++;
        return;
    }

    uint8_t lldpdu[LLDP_MAX_FRAME_LEN];
    size_t lldpdu_len = 0u;
    if (build_lldpdu(iface, src_mac, ttl, lldpdu, sizeof(lldpdu), &lldpdu_len) != ERRCODE_SUCCESS)
    {
        g_lldp_work_local->stats.tx_errors++;
        return;
    }

    uint8_t frame[LLDP_MAX_FRAME_LEN];
    size_t pos = 0u;
    memcpy(frame + pos, g_lldp_dst_mac, ETH_ALEN);
    pos += ETH_ALEN;
    memcpy(frame + pos, src_mac, ETH_ALEN);
    pos += ETH_ALEN;
    uint16_t ethertype = htons(LLDP_ETHERTYPE);
    memcpy(frame + pos, &ethertype, sizeof(ethertype));
    pos += sizeof(ethertype);
    if (pos + lldpdu_len > sizeof(frame))
    {
        g_lldp_work_local->stats.tx_errors++;
        return;
    }
    memcpy(frame + pos, lldpdu, lldpdu_len);
    pos += lldpdu_len;

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(LLDP_ETHERTYPE);
    sll.sll_ifindex = (int)if_entry->ifindex;
    sll.sll_halen = ETH_ALEN;
    memcpy(sll.sll_addr, g_lldp_dst_mac, ETH_ALEN);

    if (sendto(g_lldp_work_local->raw_fd, frame, pos, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0 && errno != EAGAIN &&
        errno != EWOULDBLOCK)
    {
        g_lldp_work_local->stats.tx_errors++;
        LOG_WARN("LLDP: send failed on %s(ifindex=%u): %s", iface->ifname, if_entry->ifindex, strerror(errno));
        return;
    }
    g_lldp_work_local->stats.tx_frames++;
}

static GString *append_hex(GString *s, uint8_t subtype, const uint8_t *data, uint16_t len)
{
    g_string_append_printf(s, "%02x", subtype);
    for (uint16_t i = 0; i < len; i++)
    {
        g_string_append_printf(s, "%02x", data[i]);
    }
    return s;
}

static char *neighbor_key(const char *ifname, const lldp_packet_t *pkt)
{
    GString *s = g_string_new(ifname ? ifname : "");
    g_string_append_c(s, '|');
    append_hex(s, pkt->chassis_id.subtype, pkt->chassis_id.data, pkt->chassis_id.len);
    g_string_append_c(s, '|');
    append_hex(s, pkt->port_id.subtype, pkt->port_id.data, pkt->port_id.len);
    return g_string_free(s, FALSE);
}

static void lldp_neighbor_id_text(const uint8_t *data, uint16_t data_len, char *buf, size_t len)
{
    if (!buf || len == 0)
    {
        return;
    }
    buf[0] = '\0';
    if (!data || data_len == 0u)
    {
        return;
    }

    int printable = 1;
    for (uint16_t i = 0; i < data_len; ++i)
    {
        if (!isprint((unsigned char)data[i]))
        {
            printable = 0;
            break;
        }
    }
    if (printable)
    {
        size_t copy = data_len < len - 1u ? data_len : len - 1u;
        memcpy(buf, data, copy);
        buf[copy] = '\0';
        return;
    }

    size_t used = 0u;
    for (uint16_t i = 0; i < data_len && used + 3u < len; ++i)
    {
        int n = snprintf(buf + used, len - used, "%s%02x", i == 0u ? "" : ":", data[i]);
        if (n < 0 || (size_t)n >= len - used)
        {
            buf[len - 1u] = '\0';
            return;
        }
        used += (size_t)n;
    }
}

static void lldp_neighbor_syslog(const lldp_neighbor_t *n, const char *state, const char *reason)
{
    if (!n)
    {
        return;
    }

    char chassis[128] = {0};
    char port[128] = {0};
    lldp_neighbor_id_text(n->chassis_id, n->chassis_len, chassis, sizeof(chassis));
    lldp_neighbor_id_text(n->port_id, n->port_len, port, sizeof(port));

    syslog_report(strcmp(state, "up") == 0 ? SYSLOG_REPORT_NOTICE : SYSLOG_REPORT_WARNING, "lldp",
                  strcmp(state, "up") == 0 ? "neighbor-up" : "neighbor-down",
                  "interface=%s neighbor=%s state=%s reason=%s chassis_subtype=%u chassis=%s port_subtype=%u port=%s "
                  "port_desc=\"%s\" ttl=%u",
                  n->ifname, n->system_name[0] ? n->system_name : "-", state, reason ? reason : "update",
                  (unsigned)n->chassis_subtype, chassis[0] ? chassis : "-", (unsigned)n->port_subtype,
                  port[0] ? port : "-", n->port_desc, (unsigned)n->ttl);
}

static gboolean upsert_neighbor(const char *ifname, const lldp_packet_t *pkt, uint64_t now)
{
    char *key = neighbor_key(ifname, pkt);
    if (!key)
    {
        return FALSE;
    }

    if (pkt->ttl == 0u)
    {
        gboolean removed = FALSE;
        lldp_neighbor_t *old = g_hash_table_lookup(g_lldp_work_local->neighbors, key);
        if (old)
        {
            lldp_neighbor_syslog(old, "down", "ttl-zero");
        }
        if (g_hash_table_remove(g_lldp_work_local->neighbors, key))
        {
            g_lldp_work_local->stats.neighbor_deletes++;
            removed = TRUE;
        }
        g_free(key);
        return removed;
    }

    lldp_neighbor_t *n = g_hash_table_lookup(g_lldp_work_local->neighbors, key);
    gboolean changed = FALSE;
    gboolean is_new = FALSE;
    if (!n)
    {
        n = g_malloc0(sizeof(*n));
        if (!n)
        {
            g_free(key);
            return FALSE;
        }
        g_hash_table_insert(g_lldp_work_local->neighbors, key, n);
        key = NULL;
        changed = TRUE;
        is_new = TRUE;
    }
    else if (n->chassis_subtype != pkt->chassis_id.subtype || n->chassis_len != pkt->chassis_id.len ||
             memcmp(n->chassis_id, pkt->chassis_id.data, pkt->chassis_id.len) != 0 ||
             n->port_subtype != pkt->port_id.subtype || n->port_len != pkt->port_id.len ||
             memcmp(n->port_id, pkt->port_id.data, pkt->port_id.len) != 0 ||
             g_strcmp0(n->system_name, pkt->system_name) != 0 || g_strcmp0(n->port_desc, pkt->port_desc) != 0 ||
             g_strcmp0(n->system_desc, pkt->system_desc) != 0 || n->caps_supported != pkt->caps_supported ||
             n->caps_enabled != pkt->caps_enabled)
    {
        changed = TRUE;
    }

    g_strlcpy(n->ifname, ifname, sizeof(n->ifname));
    n->chassis_subtype = pkt->chassis_id.subtype;
    n->chassis_len = pkt->chassis_id.len;
    memcpy(n->chassis_id, pkt->chassis_id.data, pkt->chassis_id.len);
    n->port_subtype = pkt->port_id.subtype;
    n->port_len = pkt->port_id.len;
    memcpy(n->port_id, pkt->port_id.data, pkt->port_id.len);
    n->ttl = pkt->ttl;
    n->last_seen_msec = now;
    n->expire_msec = now + ((uint64_t)pkt->ttl * 1000u);
    g_strlcpy(n->system_name, pkt->system_name, sizeof(n->system_name));
    g_strlcpy(n->port_desc, pkt->port_desc, sizeof(n->port_desc));
    g_strlcpy(n->system_desc, pkt->system_desc, sizeof(n->system_desc));
    n->caps_supported = pkt->caps_supported;
    n->caps_enabled = pkt->caps_enabled;
    g_lldp_work_local->stats.neighbor_updates++;
    if (is_new)
    {
        lldp_neighbor_syslog(n, "up", "learned");
    }
    g_free(key);
    return changed;
}

static void handle_raw_event(void)
{
    if (!g_lldp_work_local || g_lldp_work_local->raw_fd < 0)
    {
        return;
    }

    for (;;)
    {
        uint8_t frame[LLDP_MAX_FRAME_LEN];
        struct sockaddr_ll sll;
        socklen_t sll_len = sizeof(sll);
        ssize_t n = recvfrom(g_lldp_work_local->raw_fd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, &sll_len);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            LOG_WARN("LLDP: raw recv failed: %s", strerror(errno));
            break;
        }
        if ((size_t)n < ETH_HLEN || sll.sll_pkttype == PACKET_OUTGOING)
        {
            continue;
        }

        uint16_t ethertype = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
        if (ethertype != LLDP_ETHERTYPE)
        {
            continue;
        }

        const char *ifname = if_api_cache_get_logical_name((uint32_t)sll.sll_ifindex);
        if (!ifname || !ifname[0])
        {
            lldp_worker_lock();
            g_lldp_work_local->stats.rx_drops++;
            lldp_worker_unlock();
            continue;
        }

        lldp_worker_lock();
        gboolean snmp_refresh = FALSE;
        g_lldp_work_local->stats.rx_frames++;
        lldp_iface_state_t *iface = g_hash_table_lookup(g_lldp_work_local->interfaces, ifname);
        if (!iface)
        {
            const if_api_cache_entry_t *if_entry = if_api_cache_lookup(ifname);
            iface = ensure_default_iface_from_cache(if_entry);
        }
        if (!g_lldp_work_local->proto.admin_up || !iface_rx_enabled(iface))
        {
            g_lldp_work_local->stats.rx_drops++;
            lldp_worker_unlock();
            continue;
        }

        lldp_packet_t pkt;
        if (lldp_packet_parse(frame + ETH_HLEN, (size_t)n - ETH_HLEN, &pkt) == ERRCODE_SUCCESS)
        {
            snmp_refresh = upsert_neighbor(ifname, &pkt, lldp_now_msec());
        }
        else
        {
            g_lldp_work_local->stats.rx_parse_errors++;
        }
        lldp_worker_unlock();
        if (snmp_refresh)
        {
            lldp_snmp_report_refresh();
        }
    }
}

static gboolean expire_neighbors(uint64_t now)
{
    gboolean changed = FALSE;
    GHashTableIter it;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&it, g_lldp_work_local->neighbors);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        lldp_neighbor_t *n = (lldp_neighbor_t *)val;
        if (n && n->expire_msec <= now)
        {
            lldp_neighbor_syslog(n, "down", "expired");
            g_lldp_work_local->stats.neighbor_expires++;
            g_hash_table_iter_remove(&it);
            changed = TRUE;
        }
    }
    return changed;
}

static void tick(void)
{
    uint64_t expirations = 0u;
    while (read(g_lldp_work_local->timer_fd, &expirations, sizeof(expirations)) > 0)
    {
        /* drain */
    }

    uint64_t now = lldp_now_msec();
    lldp_worker_lock();
    if_api_cache_foreach(ensure_default_iface_cb, NULL);
    gboolean snmp_refresh = expire_neighbors(now);
    if (g_lldp_work_local->proto.admin_up && g_lldp_work_local->interfaces)
    {
        GHashTableIter it;
        gpointer key = NULL;
        gpointer val = NULL;
        g_hash_table_iter_init(&it, g_lldp_work_local->interfaces);
        while (g_hash_table_iter_next(&it, &key, &val))
        {
            lldp_iface_state_t *iface = (lldp_iface_state_t *)val;
            uint32_t interval = effective_tx_interval(iface);
            uint64_t due = iface->last_tx_msec + ((uint64_t)interval * 1000u);
            if (iface->last_tx_msec == 0u || now >= due)
            {
                uint32_t hold = effective_hold_multiplier(iface);
                uint32_t ttl = interval * hold;
                if (ttl > UINT16_MAX)
                {
                    ttl = UINT16_MAX;
                }
                send_on_iface(iface, (uint16_t)ttl);
                iface->last_tx_msec = now;
            }
        }
    }
    lldp_worker_unlock();
    if (snmp_refresh)
    {
        lldp_snmp_report_refresh();
    }
}

static void send_shutdown_ttl0(void)
{
    if (!g_lldp_work_local || !g_lldp_work_local->interfaces)
    {
        return;
    }
    lldp_worker_lock();
    GHashTableIter it;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&it, g_lldp_work_local->interfaces);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        send_on_iface((lldp_iface_state_t *)val, 0u);
    }
    lldp_worker_unlock();
}

static void drain_cmd_event(void)
{
    uint64_t v = 0u;
    while (read(g_lldp_work_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
        /* drain */
    }
}

static void *worker_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "lldp-worker");
    log_set_tag("lldp");

    struct epoll_event events[LLDP_MAX_EPOLL_EVENTS];
    while (g_lldp_work_local && g_lldp_work_local->running)
    {
        int n = epoll_wait(g_lldp_work_local->epoll_fd, events, LLDP_MAX_EPOLL_EVENTS, 1000);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_WARN("LLDP: epoll_wait failed: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++)
        {
            uint32_t kind = events[i].data.u32;
            if (kind == LLDP_EVT_CMD)
            {
                drain_cmd_event();
            }
            else if (kind == LLDP_EVT_TIMER)
            {
                tick();
            }
            else if (kind == LLDP_EVT_RAW)
            {
                handle_raw_event();
            }
        }
    }
    return NULL;
}

int lldp_worker_prepare(void)
{
    if (g_lldp_work_local)
    {
        return ERRCODE_SUCCESS;
    }

    g_lldp_work_local = g_malloc0(sizeof(*g_lldp_work_local));
    if (!g_lldp_work_local)
    {
        return ERRCODE_FAIL;
    }
    pthread_mutex_init(&g_lldp_work_local->lock, NULL);

    g_lldp_work_local->proto.tx_interval_sec = LLDP_DEFAULT_TX_INTERVAL_SEC;
    g_lldp_work_local->proto.hold_multiplier = LLDP_DEFAULT_HOLD_MULTIPLIER;
    g_lldp_work_local->proto.reinit_delay_sec = LLDP_DEFAULT_REINIT_DELAY_SEC;
    g_lldp_work_local->proto.tx_delay_sec = LLDP_DEFAULT_TX_DELAY_SEC;
    g_lldp_work_local->epoll_fd = -1;
    g_lldp_work_local->cmd_eventfd = -1;
    g_lldp_work_local->timer_fd = -1;
    g_lldp_work_local->raw_fd = -1;
    g_lldp_work_local->interfaces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, iface_free);
    g_lldp_work_local->neighbors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!g_lldp_work_local->interfaces || !g_lldp_work_local->neighbors)
    {
        lldp_worker_shutdown();
        return ERRCODE_FAIL;
    }

    if_api_cache_init();

    g_lldp_work_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    g_lldp_work_local->cmd_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    g_lldp_work_local->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    g_lldp_work_local->raw_fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (g_lldp_work_local->epoll_fd < 0 || g_lldp_work_local->cmd_eventfd < 0 || g_lldp_work_local->timer_fd < 0 ||
        g_lldp_work_local->raw_fd < 0)
    {
        LOG_WARN("LLDP: failed to initialize worker fds: %s", strerror(errno));
        lldp_worker_shutdown();
        return ERRCODE_FAIL;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 1;
    its.it_interval.tv_sec = 1;
    if (timerfd_settime(g_lldp_work_local->timer_fd, 0, &its, NULL) < 0)
    {
        lldp_worker_shutdown();
        return ERRCODE_FAIL;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = LLDP_EVT_CMD;
    if (epoll_ctl(g_lldp_work_local->epoll_fd, EPOLL_CTL_ADD, g_lldp_work_local->cmd_eventfd, &ev) < 0)
    {
        lldp_worker_shutdown();
        return ERRCODE_FAIL;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = LLDP_EVT_TIMER;
    if (epoll_ctl(g_lldp_work_local->epoll_fd, EPOLL_CTL_ADD, g_lldp_work_local->timer_fd, &ev) < 0)
    {
        lldp_worker_shutdown();
        return ERRCODE_FAIL;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = LLDP_EVT_RAW;
    if (epoll_ctl(g_lldp_work_local->epoll_fd, EPOLL_CTL_ADD, g_lldp_work_local->raw_fd, &ev) < 0)
    {
        lldp_worker_shutdown();
        return ERRCODE_FAIL;
    }

    g_lldp_work_local->running = 1;
    return ERRCODE_SUCCESS;
}

int lldp_worker_launch(void)
{
    if (!g_lldp_work_local)
    {
        return ERRCODE_FAIL;
    }
    if (pthread_create(&g_lldp_work_local->thread, NULL, worker_thread_fn, NULL) != 0)
    {
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

void lldp_worker_shutdown(void)
{
    if (!g_lldp_work_local)
    {
        return;
    }

    if (g_lldp_work_local->running && g_lldp_work_local->thread != 0)
    {
        send_shutdown_ttl0();
        g_lldp_work_local->running = 0;
        if (g_lldp_work_local->cmd_eventfd >= 0)
        {
            uint64_t one = 1u;
            (void)write(g_lldp_work_local->cmd_eventfd, &one, sizeof(one));
        }
        pthread_join(g_lldp_work_local->thread, NULL);
        g_lldp_work_local->thread = 0;
    }

    close_fd(&g_lldp_work_local->raw_fd);
    close_fd(&g_lldp_work_local->timer_fd);
    close_fd(&g_lldp_work_local->cmd_eventfd);
    close_fd(&g_lldp_work_local->epoll_fd);

    if (g_lldp_work_local->interfaces)
    {
        g_hash_table_destroy(g_lldp_work_local->interfaces);
        g_lldp_work_local->interfaces = NULL;
    }
    if (g_lldp_work_local->neighbors)
    {
        g_hash_table_destroy(g_lldp_work_local->neighbors);
        g_lldp_work_local->neighbors = NULL;
    }
    if_api_cache_cleanup();
    pthread_mutex_destroy(&g_lldp_work_local->lock);
    g_free(g_lldp_work_local);
    g_lldp_work_local = NULL;
}

int lldp_worker_dispatch_apply(lldp_apply_cmd_t *apply)
{
    if (!g_lldp_work_local || !apply)
    {
        return ERRCODE_FAIL;
    }

    lldp_worker_lock();
    apply->rc = ERRCODE_SUCCESS;
    switch (apply->op)
    {
        case LLDP_APPLY_OP_PROTO_SET:
            g_lldp_work_local->proto = apply->u.proto;
            break;

        case LLDP_APPLY_OP_IF_SET:
        {
            const char *ifname = apply->u.if_set.ifname;
            if (!ifname[0])
            {
                apply->rc = ERRCODE_FAIL;
                break;
            }
            lldp_iface_state_t *iface = g_hash_table_lookup(g_lldp_work_local->interfaces, ifname);
            if (!iface)
            {
                iface = g_malloc0(sizeof(*iface));
                if (!iface)
                {
                    apply->rc = ERRCODE_FAIL;
                    break;
                }
                g_strlcpy(iface->ifname, ifname, sizeof(iface->ifname));
                g_hash_table_insert(g_lldp_work_local->interfaces, g_strdup(iface->ifname), iface);
            }
            iface->enabled = apply->u.if_set.enabled ? 1u : 0u;
            iface->admin_status = apply->u.if_set.admin_status ? apply->u.if_set.admin_status : LLDP_IF_ADMIN_TX_RX;
            iface->configured = 1u;
            iface->tx_interval_sec = apply->u.if_set.tx_interval_sec;
            iface->hold_multiplier = apply->u.if_set.hold_multiplier;
            g_strlcpy(iface->port_desc, apply->u.if_set.port_desc, sizeof(iface->port_desc));

            const if_api_cache_entry_t *if_entry = if_api_cache_lookup(iface->ifname);
            if (if_entry)
            {
                iface->ifindex = if_entry->ifindex;
                iface->link_up = if_entry->link_up ? 1u : 0u;
            }
            break;
        }

        case LLDP_APPLY_OP_IF_DEL:
            if (apply->u.if_del.ifname[0])
            {
                g_hash_table_remove(g_lldp_work_local->interfaces, apply->u.if_del.ifname);
            }
            break;

        default:
            apply->rc = ERRCODE_FAIL;
            break;
    }
    lldp_worker_unlock();
    lldp_snmp_report_refresh();
    return apply->rc;
}

int lldp_worker_post_show_cli(dev_ipc_message_t *msg)
{
    return lldp_show_handle_msg(msg);
}

static void refresh_iface_from_event(const if_event_msg_t *e)
{
    if (!e || !g_lldp_work_local || !g_lldp_work_local->interfaces)
    {
        return;
    }
    lldp_iface_state_t *iface = g_hash_table_lookup(g_lldp_work_local->interfaces, e->logical_name);
    if (!iface)
    {
        const if_api_cache_entry_t *if_entry = if_api_cache_lookup(e->logical_name);
        iface = ensure_default_iface_from_cache(if_entry);
    }
    if (!iface)
    {
        return;
    }

    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(iface->ifname);
    iface->ifindex = if_entry ? if_entry->ifindex : 0u;
    iface->link_up = e->link_up ? 1u : 0u;
    if (iface->link_up)
    {
        iface->last_tx_msec = 0u;
    }
}

int lldp_worker_post_if_event(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    if_api_cache_on_event(msg);
    if (msg->payload && msg->payload_len >= sizeof(if_event_msg_t))
    {
        const if_event_msg_t *e = (const if_event_msg_t *)msg->payload;
        if (e->event == IF_EVENT_LINK_UP || e->event == IF_EVENT_LINK_DOWN)
        {
            lldp_worker_lock();
            refresh_iface_from_event(e);
            lldp_worker_unlock();
            lldp_snmp_report_refresh();
        }
    }
    dev_ipc_message_free(msg);
    return ERRCODE_SUCCESS;
}

int lldp_worker_post_if_down(void)
{
    if_api_cache_cleanup();
    if_api_cache_init();
    lldp_worker_lock();
    if (g_lldp_work_local && g_lldp_work_local->interfaces)
    {
        GHashTableIter it;
        gpointer key = NULL;
        gpointer val = NULL;
        g_hash_table_iter_init(&it, g_lldp_work_local->interfaces);
        while (g_hash_table_iter_next(&it, &key, &val))
        {
            lldp_iface_state_t *iface = (lldp_iface_state_t *)val;
            iface->ifindex = 0u;
            iface->link_up = 0u;
            iface->last_tx_msec = 0u;
        }
    }
    if (g_lldp_work_local && g_lldp_work_local->neighbors)
    {
        GHashTableIter it;
        gpointer key = NULL;
        gpointer val = NULL;
        g_hash_table_iter_init(&it, g_lldp_work_local->neighbors);
        while (g_hash_table_iter_next(&it, &key, &val))
        {
            lldp_neighbor_syslog((const lldp_neighbor_t *)val, "down", "if-down");
        }
        g_hash_table_remove_all(g_lldp_work_local->neighbors);
    }
    lldp_worker_unlock();
    lldp_snmp_report_refresh();
    LOG_INFO("LLDP: IF down, cache cleared");
    return ERRCODE_SUCCESS;
}
