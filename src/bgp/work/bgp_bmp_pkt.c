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
#include "bgp_protocol.h"
#include "bgp_worker.h"
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
                /* 非阻塞 socket 发不出去，丢弃剩余部分 */
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
 * @brief 写入 BMP common header（6 字节）到 buf
 * @param buf      输出缓冲区（至少 6 字节）
 * @param msg_len  整个 BMP 消息长度（含公共头）
 * @param msg_type 消息类型
 */
static void bmp_write_common_hdr(uint8_t *buf, uint32_t msg_len, uint8_t msg_type)
{
    buf[0] = BGP_BMP_VERSION;
    uint32_t nl = htonl(msg_len);
    memcpy(buf + 1, &nl, 4);
    buf[5] = msg_type;
}

/**
 * @brief 写入 BMP per-peer header（42 字节）到 buf
 * @param buf  输出缓冲区（至少 42 字节）
 * @param sess BGP 会话
 */
static void bmp_write_per_peer_hdr(uint8_t *buf, bgp_session_t *sess)
{
    memset(buf, 0, BGP_BMP_PER_PEER_HDR_LEN);

    /* byte 0: peer type = Global Instance */
    buf[0] = BGP_BMP_PEER_TYPE_GLOBAL;

    /* byte 1: peer flags */
    uint8_t flags = 0;
    if (sess->neighbor_addr.family == AF_INET6)
    {
        flags |= BGP_BMP_PEER_FLAG_V;
    }
    /* Pre-policy (L=0) */
    buf[1] = flags;

    /* byte 2-9: Peer Distinguisher (0 for global) */

    /* byte 10-25: Peer Address（16 字节，IPv4 放在最后 4 字节） */
    if (sess->neighbor_addr.family == AF_INET)
    {
        memcpy(buf + 22, &sess->neighbor_addr.u.v4, 4);
    }
    else if (sess->neighbor_addr.family == AF_INET6)
    {
        memcpy(buf + 10, &sess->neighbor_addr.u.v6, 16);
    }

    /* byte 26-29: Peer AS（网络序） */
    uint32_t as_be = htonl(sess->remote_as);
    memcpy(buf + 26, &as_be, 4);

    /* byte 30-33: Peer BGP ID（网络序） */
    uint32_t id_be = htonl(sess->remote_id);
    memcpy(buf + 30, &id_be, 4);

    /* byte 34-37: Timestamp (seconds) */
    /* byte 38-41: Timestamp (microseconds) */
    if (sess->established_at_usec > 0)
    {
        int64_t ts = sess->established_at_usec;
        uint32_t ts_sec = htonl((uint32_t)(ts / 1000000));
        uint32_t ts_usec = htonl((uint32_t)(ts % 1000000));
        memcpy(buf + 34, &ts_sec, 4);
        memcpy(buf + 38, &ts_usec, 4);
    }
}

// ============================================================================
// Initiation Message（RFC 7854 §4.3）
// ============================================================================

/**
 * @brief 写一条 Initiation/Termination TLV
 * @param buf  输出位置
 * @param type TLV type（2 字节 BE）
 * @param str  值字符串
 * @return 写入的字节数（4 + strlen）
 */
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

    /* sysName TLV */
    char hostname[64] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0)
    {
        g_strlcpy(hostname, "netnexus", sizeof(hostname));
    }
    offset += bmp_write_info_tlv(buf + offset, BGP_BMP_INIT_TLV_SYS_NAME, hostname);

    /* sysDescr TLV */
    offset += bmp_write_info_tlv(buf + offset, BGP_BMP_INIT_TLV_SYS_DESCR, "NetNexus BGP BMP Reporter");

    /* 字符串 TLV：BMP 实例名 */
    char info[96];
    snprintf(info, sizeof(info), "BMP instance: %s", inst->name);
    offset += bmp_write_info_tlv(buf + offset, BGP_BMP_INIT_TLV_STRING, info);

    /* 回填公共头 */
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

    /* Reason TLV (type=1, length=2, value=reason_code BE) */
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

int bgp_bmp_pkt_send_peer_up(bgp_bmp_instance_t *inst, bgp_session_t *sess)
{
    uint8_t buf[BMP_SEND_BUF_MAX];
    uint32_t offset = BGP_BMP_COMMON_HDR_LEN;

    /* Per-Peer Header */
    bmp_write_per_peer_hdr(buf + offset, sess);
    offset += BGP_BMP_PER_PEER_HDR_LEN;

    /* Local Address（16 字节） */
    memset(buf + offset, 0, 16);
    /* 使用 source_addr（如果有），否则留零 */
    if (sess->source_addr.family == AF_INET)
    {
        memcpy(buf + offset + 12, &sess->source_addr.u.v4, 4);
    }
    else if (sess->source_addr.family == AF_INET6)
    {
        memcpy(buf + offset, &sess->source_addr.u.v6, 16);
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

    /*
     * Sent OPEN Message + Received OPEN Message
     * 当前实现未缓存原始 OPEN PDU，发送空 OPEN（各 29 字节最小 OPEN）
     * 最小 BGP OPEN: 16(marker) + 2(len) + 1(type) + 10(OPEN fixed) = 29 字节
     */
    uint8_t min_open[29];
    memset(min_open, 0xFF, 16); /* marker */
    uint16_t open_len_be = htons(29);
    memcpy(min_open + 16, &open_len_be, 2);
    min_open[18] = 1; /* type = OPEN */
    min_open[19] = 4; /* version = 4 */

    /* Sent OPEN: 本地 AS + Hold Time + BGP ID */
    uint16_t local_as16 = 0;
    uint32_t local_as = 0;
    uint16_t local_hold = 0;
    uint32_t local_bgp_id = 0;

    if (g_bgp_work_local->protocol)
    {
        local_as = g_bgp_work_local->protocol->as_number;
        local_as16 = (local_as > 65535) ? htons(23456) : htons((uint16_t)local_as);
    }
    if (sess->vrf)
    {
        local_hold = htons(sess->vrf->hold_time);
        local_bgp_id = htonl(sess->vrf->router_id);
    }

    memcpy(min_open + 20, &local_as16, 2);
    memcpy(min_open + 22, &local_hold, 2);
    memcpy(min_open + 24, &local_bgp_id, 4);
    min_open[28] = 0; /* opt params len */

    memcpy(buf + offset, min_open, 29);
    offset += 29;

    /* Received OPEN: 对端 AS + 协商 Hold Time + 对端 BGP ID */
    uint16_t remote_as16 = (sess->remote_as > 65535) ? htons(23456) : htons((uint16_t)sess->remote_as);
    uint16_t remote_hold = htons(sess->remote_hold);
    uint32_t remote_bgp_id = htonl(sess->remote_id);

    memcpy(min_open + 20, &remote_as16, 2);
    memcpy(min_open + 22, &remote_hold, 2);
    memcpy(min_open + 24, &remote_bgp_id, 4);
    min_open[28] = 0;

    memcpy(buf + offset, min_open, 29);
    offset += 29;

    /* 回填公共头 */
    bmp_write_common_hdr(buf, offset, BGP_BMP_MSG_PEER_UP);

    if (bgp_bmp_send_raw(inst, buf, offset) == 0)
    {
        inst->peer_up_sent++;
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BMP '%s': Peer Up sent for %s (%u bytes)", inst->name, addr_str, offset);
        return 0;
    }
    return -1;
}

// ============================================================================
// Peer Down Notification（RFC 7854 §4.9）
// ============================================================================

int bgp_bmp_pkt_send_peer_down(bgp_bmp_instance_t *inst, bgp_session_t *sess, uint8_t reason)
{
    uint8_t buf[BMP_SEND_BUF_MAX];
    uint32_t offset = BGP_BMP_COMMON_HDR_LEN;

    /* Per-Peer Header */
    bmp_write_per_peer_hdr(buf + offset, sess);
    offset += BGP_BMP_PER_PEER_HDR_LEN;

    /* Reason（1 字节） */
    buf[offset] = reason;
    offset += 1;

    /* 回填公共头 */
    bmp_write_common_hdr(buf, offset, BGP_BMP_MSG_PEER_DOWN);

    if (bgp_bmp_send_raw(inst, buf, offset) == 0)
    {
        inst->peer_down_sent++;
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BMP '%s': Peer Down sent for %s reason=%u (%u bytes)", inst->name, addr_str, reason, offset);
        return 0;
    }
    return -1;
}

// ============================================================================
// Route Monitoring（RFC 7854 §4.6）
// ============================================================================

int bgp_bmp_pkt_send_route_monitoring(bgp_bmp_instance_t *inst, bgp_session_t *sess, const uint8_t *bgp_pdu,
                                      uint16_t pdu_len)
{
    if (!bgp_pdu || pdu_len == 0)
    {
        return -1;
    }

    /* 公共头 + Per-Peer 头 + BGP PDU */
    uint32_t total_len = BGP_BMP_COMMON_HDR_LEN + BGP_BMP_PER_PEER_HDR_LEN + pdu_len;

    uint8_t hdr[BGP_BMP_COMMON_HDR_LEN + BGP_BMP_PER_PEER_HDR_LEN];
    bmp_write_common_hdr(hdr, total_len, BGP_BMP_MSG_ROUTE_MONITORING);
    bmp_write_per_peer_hdr(hdr + BGP_BMP_COMMON_HDR_LEN, sess);

    /* 使用 writev 避免拷贝 BGP PDU */
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

    /*
     * Stats Report 不需要 per-peer header（统计是全局的）。
     * 但 RFC 7854 规定 Stats Report 需要 per-peer header。
     * 简化实现：发送一个全零 per-peer + Stats Count = 0 的空报告。
     */
    memset(buf + offset, 0, BGP_BMP_PER_PEER_HDR_LEN);
    offset += BGP_BMP_PER_PEER_HDR_LEN;

    /* Stats Count（4 字节 BE）= 0 */
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
