/**
 * @file   bgp_instance.h
 * @brief  BGP 地址族实例结构定义（无 VRF 字段，按 afi/safi 索引）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef BGP_INSTANCE_H
#define BGP_INSTANCE_H

#include <glib.h>
#include <stddef.h>

#include "bgp_peer.h"

/**
 * @brief BGP 地址族实例（持有该 AF 下所有已使能邻居的 bgp_peer_t 所有权）
 */
typedef struct bgp_instance
{
    bgp_afi_t afi;         /**< 地址族 */
    bgp_safi_t safi;       /**< 子地址族 */
    GHashTable *peer_hash; /**< addr_str(gchar*) -> bgp_peer_t*（持有所有权） */
} bgp_instance_t;

/**
 * @brief 构造地址族实例键字符串（如 "1-1" 表示 IPv4 Unicast）
 * @param afi  地址族
 * @param safi 子地址族
 * @param buf  输出缓冲区
 * @param sz   缓冲区大小
 */
void bgp_instance_make_key(bgp_afi_t afi, bgp_safi_t safi, char *buf, size_t sz);

/**
 * @brief 创建地址族实例结构
 * @param afi  地址族
 * @param safi 子地址族
 * @return 新建的 bgp_instance_t 指针
 */
bgp_instance_t *bgp_instance_create(bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 销毁地址族实例结构（同时销毁所有 peer_hash 中的 bgp_peer_t）
 * @param inst bgp_instance_t 指针（允许为 NULL）
 */
void bgp_instance_destroy(bgp_instance_t *inst);

#endif /* BGP_INSTANCE_H */
