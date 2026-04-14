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

/* 前向声明，避免循环包含 */
typedef struct bgp_vrf bgp_vrf_t;
typedef struct bgp_instance bgp_instance_t;
typedef struct bgp_nh_subgroup bgp_nh_subgroup_t;

/** BGP per-AF peer：在某地址族下使能的邻居实例 */
typedef struct bgp_peer
{
    net_addr_t addr;      /**< 邻居 IP 地址 */
    bool established;     /**< 已通过 OPEN 协商激活 */
    bgp_vrf_t *vrf;       /**< 所属 VRF（借用引用，不持有所有权） */
    bgp_instance_t *inst; /**< 所属 AF 实例（借用引用，不持有所有权） */
    GList *subgroups;     /**< bgp_nh_subgroup_t* 借用引用列表（可同时归属多个子组） */
} bgp_peer_t;

/**
 * @brief 创建 per-AF peer 实例
 * @param vrf  所属 VRF（借用引用）
 * @param inst 所属 AF 实例（借用引用）
 * @param addr 邻居 IP 地址
 * @return 新建的 peer 指针
 */
bgp_peer_t *bgp_peer_create(bgp_vrf_t *vrf, bgp_instance_t *inst, const net_addr_t *addr);

/**
 * @brief 销毁 per-AF peer 实例
 * @param peer peer 指针（允许为 NULL）
 */
void bgp_peer_destroy(bgp_peer_t *peer);

#endif /* BGP_PEER_H */
