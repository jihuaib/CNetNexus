/**
 * @file   bgp_peer.h
 * @brief  BGP per-AF peer（bgp_peer_t）结构定义
 * @author jhb
 * @date   2026/03/02
 */
#ifndef BGP_PEER_H
#define BGP_PEER_H

#include <glib.h>
#include <stdbool.h>

#include "net_addr.h"

/** 地址族标识符（RFC 1700） */
typedef enum bgp_afi
{
    BGP_AFI_IPV4 = 1, /**< IPv4 */
    BGP_AFI_IPV6 = 2, /**< IPv6 */
} bgp_afi_t;

/** 子地址族标识符 */
typedef enum bgp_safi
{
    BGP_SAFI_UNICAST = 1, /**< 单播 */
} bgp_safi_t;

/** BGP per-AF peer：在某地址族下使能的邻居实例 */
typedef struct bgp_peer
{
    net_addr_t addr;  /**< 邻居 IP 地址 */
    bgp_afi_t afi;    /**< 地址族，方便 OPEN 构建 MP capability */
    bgp_safi_t safi;  /**< 子地址族 */
    bool established; /**< 已通过 OPEN 协商激活 */
} bgp_peer_t;

/**
 * @brief 创建 per-AF peer 实例
 * @param afi  地址族
 * @param safi 子地址族
 * @param addr 邻居 IP 地址
 * @return 新建的 peer 指针
 */
bgp_peer_t *bgp_peer_create(bgp_afi_t afi, bgp_safi_t safi, const net_addr_t *addr);

/**
 * @brief 销毁 per-AF peer 实例
 * @param peer peer 指针（允许为 NULL）
 */
void bgp_peer_destroy(bgp_peer_t *peer);

#endif /* BGP_PEER_H */
