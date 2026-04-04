/**
 * @file   bgp_bmp_pkt.h
 * @brief  BMP 报文构建接口（RFC 7854 wire format）
 * @author jhb
 * @date   2026/03/30
 */
#ifndef BGP_BMP_PKT_H
#define BGP_BMP_PKT_H

#include <stdint.h>

#include "bgp_bmp.h"
#include "bgp_bmp_thread.h"

/**
 * @brief 向 BMP 实例发送原始字节（封装 write 循环）
 * @return 0 成功，-1 失败
 */
int bgp_bmp_send_raw(bgp_bmp_instance_t *inst, const uint8_t *data, uint32_t len);

/**
 * @brief 构建并发送 Initiation 消息
 */
int bgp_bmp_pkt_send_initiation(bgp_bmp_instance_t *inst);

/**
 * @brief 构建并发送 Termination 消息
 */
int bgp_bmp_pkt_send_termination(bgp_bmp_instance_t *inst, uint16_t reason);

/**
 * @brief 构建并发送 Peer Up 通知
 * @param inst BMP 实例
 * @param peer 邻居信息快照
 */
int bgp_bmp_pkt_send_peer_up(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer);

/**
 * @brief 构建并发送 Peer Down 通知
 * @param inst   BMP 实例
 * @param peer   邻居信息快照
 * @param reason Peer Down 原因码（BGP_BMP_PEER_DOWN_*）
 */
int bgp_bmp_pkt_send_peer_down(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer, uint8_t reason);

/**
 * @brief 构建并发送 Route Monitoring 消息
 * @param inst    BMP 实例
 * @param peer    邻居信息快照
 * @param bgp_pdu 原始 BGP UPDATE PDU（含 19 字节 BGP header）
 * @param pdu_len PDU 长度
 */
int bgp_bmp_pkt_send_route_monitoring(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer, const uint8_t *bgp_pdu,
                                      uint16_t pdu_len);

/**
 * @brief 构建并发送 Stats Report 消息
 */
int bgp_bmp_pkt_send_stats_report(bgp_bmp_instance_t *inst);

#endif /* BGP_BMP_PKT_H */
