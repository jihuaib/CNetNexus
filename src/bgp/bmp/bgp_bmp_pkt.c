/**
 * @file   bgp_bmp_pkt.c
 * @brief  BMP 报文构建实现（RFC 7854 wire format）
 * @author jhb
 * @date   2026/03/30
 */
#include "bgp_bmp_pkt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "bgp_bmp.h"
#include "bgp_bmp_thread.h"
#include "log.h"
#include "net_addr.h"

/** 发送缓冲区上限 */
#define BMP_SEND_BUF_MAX 4096

// ============================================================================
// 辅助：原始发送
// ============================================================================

int bgp_bmp_send_raw(bgp_bmp_instance_t *inst, const uint8_t *data, uint32_t len)
{
    if (!inst || inst->fd < 0 || !data || len == 0)
    {
        return -1;
    }

    uint32_t sent = 0;
    while (sent < len)
    {
        ssize_t n = write(inst->fd, data + sent, len - sent);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                LOG_WARN("BMP '%s': send buffer full, %u/%u bytes sent", inst->name, sent, len);
                return -1;
            }
            LOG_PERROR("BMP '%s': write failed", inst->name);
            return -1;
        }
        sent += (uint32_t)n;
    }
    return 0;
}

// ============================================================================
// 辅助：公共头 & Per-Peer 头
// ============================================================================

/**
 * @brief 写入 BMP common header（6 字节）
 */
static void bmp_write_common_hdr(uint8_t *buf, uint32_t msg_len, uint8_t msg_type)
{
    buf[0] = BGP_BMP_VERSION;
    uint32_t nl = htonl(msg_len);
    memcpy(buf + 1, &nl, 4);
    buf[5] = msg_type;
}

/**
 * @brief 写入 BMP per-peer header（42 字节），使用 peer_info 快照
 */
static void bmp_write_per_peer_hdr(uint8_t *buf, const bgp_bmp_peer_info_t *peer)
{
    memset(buf, 0, BGP_BMP_PER_PEER_HDR_LEN);

    /* byte 0: peer type = Global Instance */
    buf[0] = BGP_BMP_PEER_TYPE_GLOBAL;

    /* byte 1: peer flags */
    uint8_t flags = 0;
    if (peer->neighbor_addr.family == AF_INET6)
    {
        flags |= BGP_BMP_PEER_FLAG_V;
    }
    buf[1] = flags;

    /* byte 2-9: Peer Distinguisher (0 for global) */

    /* byte 10-25: Peer Address（16 字节，IPv4 放在最后 4 字节） */
    if (peer->neighbor_addr.family == AF_INET)
    {
        memcpy(buf + 22, &peer->neighbor_addr.u.v4, 4);
    }
    else if (peer->neighbor_addr.family == AF_INET6)
    {
        memcpy(buf + 10, &peer->neighbor_addr.u.v6, 16);
    }

    /* byte 26-29: Peer AS（网络序） */
    uint32_t as_be = htonl(peer->remote_as);
    memcpy(buf + 26, &as_be, 4);

    /* byte 30-33: Peer BGP ID（网络序） */
    uint32_t id_be = htonl(peer->remote_id);
    memcpy(buf + 30, &id_be, 4);

    /* byte 34-37: Timestamp (seconds) */
    /* byte 38-41: Timestamp (microseconds) */
    if (peer->established_at_usec > 0)
    {
        int64_t ts = peer->established_at_usec;
        uint32_t ts_sec = htonl((uint32_t)(ts / 1000000));
        uint32_t ts_usec = htonl((uint32_t)(ts % 1000000));
        memcpy(buf + 34, &ts_sec, 4);
        memcpy(buf + 38, &ts_usec, 4);
    }
}

// ============================================================================
// Initiation Message（RFC 7854 §4.3）
// ============================================================================

static uint32_t bmp_write_info_tlv(uint8_t *buf, uint16_t type, const char *str)
{
    uint16_t len = (uint16_t)strlen(str);
    uint16_t type_be = htons(type);
    uint16_t len_be = htons(len);
    memcpy(buf, &type_be, 2);
    memcpy(buf + 2, &len_be, 2);
    if (len > 0)
    {
        memcpy(buf + 4, str, len);
    }
    return 4 + len;
}

int bgp_bmp_pkt_send_initiation(bgp_bmp_instance_t *inst)
{
    uint8_t buf[BMP_SEND_BUF_MAX];
    uint32_t offset = BGP_BMP_COMMON_HDR_LEN;

    char hostname[64] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0)
    {
        g_strlcpy(hostname, "netnexus", sizeof(hostname));
    }
    offset += bmp_write_info_tlv(buf + offset, BGP_BMP_INIT_TLV_SYS_NAME, hostname);
    offset += bmp_write_info_tlv(buf + offset, BGP_BMP_INIT_TLV_SYS_DESCR, "NetNexus BGP BMP Reporter");

    char info[96];
    snprintf(info, sizeof(info), "BMP instance: %s", inst->name);
    offset += bmp_write_info_tlv(buf + offset, BGP_BMP_INIT_TLV_STRING, info);

    bmp_write_common_hdr(buf, offset, BGP_BMP_MSG_INITIATION);

    if (bgp_bmp_send_raw(inst, buf, offset) == 0)
    {
        inst->initiation_sent++;
        LOG_INFO("BMP '%s': Initiation message sent (%u bytes)", inst->name, offset);
        return 0;
    }
    return -1;
}

// ============================================================================
// Termination Message（RFC 7854 §4.5）
// ============================================================================

int bgp_bmp_pkt_send_termination(bgp_bmp_instance_t *inst, uint16_t reason)
{
    uint8_t buf[BMP_SEND_BUF_MAX];
    uint32_t offset = BGP_BMP_COMMON_HDR_LEN;

    uint16_t type_be = htons(BGP_BMP_TERM_TLV_REASON);
    uint16_t len_be = htons(2);
    uint16_t reason_be = htons(reason);
    memcpy(buf + offset, &type_be, 2);
    offset += 2;
    memcpy(buf + offset, &len_be, 2);
    offset += 2;
    memcpy(buf + offset, &reason_be, 2);
    offset += 2;

    bmp_write_common_hdr(buf, offset, BGP_BMP_MSG_TERMINATION);

    if (bgp_bmp_send_raw(inst, buf, offset) == 0)
    {
        LOG_INFO("BMP '%s': Termination message sent (reason=%u)", inst->name, reason);
        return 0;
    }
    return -1;
}

// ============================================================================
// Peer Up Notification（RFC 7854 §4.10）
// ============================================================================

int bgp_bmp_pkt_send_peer_up(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer)
{
    uint8_t buf[BMP_SEND_BUF_MAX];
    uint32_t offset = BGP_BMP_COMMON_HDR_LEN;

    /* Per-Peer Header */
    bmp_write_per_peer_hdr(buf + offset, peer);
    offset += BGP_BMP_PER_PEER_HDR_LEN;

    /* Local Address（16 字节） */
    memset(buf + offset, 0, 16);
    if (peer->source_addr.family == AF_INET)
    {
        memcpy(buf + offset + 12, &peer->source_addr.u.v4, 4);
    }
    else if (peer->source_addr.family == AF_INET6)
    {
        memcpy(buf + offset, &peer->source_addr.u.v6, 16);
    }
    offset += 16;

    /* Local Port（2 字节 BE）- BGP 179 */
    uint16_t local_port_be = htons(179);
    memcpy(buf + offset, &local_port_be, 2);
    offset += 2;

    /* Remote Port（2 字节 BE）- BGP 179 */
    uint16_t remote_port_be = htons(179);
    memcpy(buf + offset, &remote_port_be, 2);
    offset += 2;

    /* 最小 BGP OPEN: 16(marker) + 2(len) + 1(type) + 10(OPEN fixed) = 29 字节 */
    uint8_t min_open[29];
    memset(min_open, 0xFF, 16); /* marker */
    uint16_t open_len_be = htons(29);
    memcpy(min_open + 16, &open_len_be, 2);
    min_open[18] = 1; /* type = OPEN */
    min_open[19] = 4; /* version = 4 */

    /* Sent OPEN: 本地 AS + Hold Time + BGP ID */
    uint32_t local_as = peer->local_as;
    uint16_t local_as16 = (local_as > 65535) ? htons(23456) : htons((uint16_t)local_as);
    uint16_t local_hold = htons(peer->local_hold_time);
    uint32_t local_bgp_id = htonl(peer->local_router_id);

    memcpy(min_open + 20, &local_as16, 2);
    memcpy(min_open + 22, &local_hold, 2);
    memcpy(min_open + 24, &local_bgp_id, 4);
    min_open[28] = 0; /* opt params len */

    memcpy(buf + offset, min_open, 29);
    offset += 29;

    /* Received OPEN: 对端 AS + 协商 Hold Time + 对端 BGP ID */
    uint16_t remote_as16 = (peer->remote_as > 65535) ? htons(23456) : htons((uint16_t)peer->remote_as);
    uint16_t remote_hold = htons(peer->remote_hold);
    uint32_t remote_bgp_id = htonl(peer->remote_id);

    memcpy(min_open + 20, &remote_as16, 2);
    memcpy(min_open + 22, &remote_hold, 2);
    memcpy(min_open + 24, &remote_bgp_id, 4);
    min_open[28] = 0;

    memcpy(buf + offset, min_open, 29);
    offset += 29;

    bmp_write_common_hdr(buf, offset, BGP_BMP_MSG_PEER_UP);

    if (bgp_bmp_send_raw(inst, buf, offset) == 0)
    {
        inst->peer_up_sent++;
        char addr_str[64];
        net_addr_to_str(&peer->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BMP '%s': Peer Up sent for %s (%u bytes)", inst->name, addr_str, offset);
        return 0;
    }
    return -1;
}

// ============================================================================
// Peer Down Notification（RFC 7854 §4.9）
// ============================================================================

int bgp_bmp_pkt_send_peer_down(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer, uint8_t reason)
{
    uint8_t buf[BMP_SEND_BUF_MAX];
    uint32_t offset = BGP_BMP_COMMON_HDR_LEN;

    bmp_write_per_peer_hdr(buf + offset, peer);
    offset += BGP_BMP_PER_PEER_HDR_LEN;

    buf[offset] = reason;
    offset += 1;

    bmp_write_common_hdr(buf, offset, BGP_BMP_MSG_PEER_DOWN);

    if (bgp_bmp_send_raw(inst, buf, offset) == 0)
    {
        inst->peer_down_sent++;
        char addr_str[64];
        net_addr_to_str(&peer->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BMP '%s': Peer Down sent for %s reason=%u (%u bytes)", inst->name, addr_str, reason, offset);
        return 0;
    }
    return -1;
}

// ============================================================================
// Route Monitoring（RFC 7854 §4.6）
// ============================================================================

int bgp_bmp_pkt_send_route_monitoring(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer, const uint8_t *bgp_pdu,
                                      uint16_t pdu_len)
{
    if (!bgp_pdu || pdu_len == 0)
    {
        return -1;
    }

    uint32_t total_len = BGP_BMP_COMMON_HDR_LEN + BGP_BMP_PER_PEER_HDR_LEN + pdu_len;

    uint8_t hdr[BGP_BMP_COMMON_HDR_LEN + BGP_BMP_PER_PEER_HDR_LEN];
    bmp_write_common_hdr(hdr, total_len, BGP_BMP_MSG_ROUTE_MONITORING);
    bmp_write_per_peer_hdr(hdr + BGP_BMP_COMMON_HDR_LEN, peer);

    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = (void *)(uintptr_t)bgp_pdu;
    iov[1].iov_len = pdu_len;

    ssize_t n = writev(inst->fd, iov, 2);
    if (n < 0)
    {
        LOG_PERROR("BMP '%s': writev Route Monitoring failed", inst->name);
        return -1;
    }
    if ((uint32_t)n < total_len)
    {
        LOG_WARN("BMP '%s': Route Monitoring partial write %zd/%u", inst->name, n, total_len);
        return -1;
    }

    inst->route_monitor_sent++;
    return 0;
}

// ============================================================================
// Stats Report（RFC 7854 §4.8）
// ============================================================================

int bgp_bmp_pkt_send_stats_report(bgp_bmp_instance_t *inst)
{
    uint8_t buf[BMP_SEND_BUF_MAX];
    uint32_t offset = BGP_BMP_COMMON_HDR_LEN;

    memset(buf + offset, 0, BGP_BMP_PER_PEER_HDR_LEN);
    offset += BGP_BMP_PER_PEER_HDR_LEN;

    uint32_t count_be = htonl(0);
    memcpy(buf + offset, &count_be, 4);
    offset += 4;

    bmp_write_common_hdr(buf, offset, BGP_BMP_MSG_STATS_REPORT);

    if (bgp_bmp_send_raw(inst, buf, offset) == 0)
    {
        inst->stats_report_sent++;
        return 0;
    }
    return -1;
}
