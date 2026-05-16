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
#include <stdint.h>

#include "net_addr.h"

/** per-peer 配置标记位 */
#define BGP_PEER_FLAG_RR_CLIENT (1U << 0) /**< 标记为 Route Reflector 客户端（RFC 4456） */

/* 前向声明，避免循环包含 */
typedef struct bgp_vrf bgp_vrf_t;
typedef struct bgp_instance bgp_instance_t;
typedef struct bgp_nh_subgroup bgp_nh_subgroup_t;

/**
 * @brief BGP per-AF peer 状态
 *
 * 与 session FSM 状态解耦：session 虽已 ESTABLISHED，但对端 OPEN
 * 未携带本 AF 的 MP 能力时，peer 停留在 NOT_NEGOTIATED；其他
 * 非 ESTABLISHED 情形 peer 统一为 IDLE，由 show 路径回落到 session 态显示。
 */
typedef enum bgp_peer_state
{
    BGP_PEER_STATE_IDLE = 0,           /**< session 未 ESTABLISHED 或未协商本 AF 前的初态 */
    BGP_PEER_STATE_NOT_NEGOTIATED = 1, /**< session ESTABLISHED 但对端未协商本 AF */
    BGP_PEER_STATE_ESTABLISHED = 2,    /**< session ESTABLISHED 且本 AF 已协商 */
} bgp_peer_state_t;

/** BGP per-AF peer：在某地址族下使能的邻居实例 */
typedef struct bgp_peer
{
    net_addr_t addr;        /**< 邻居 IP 地址 */
    bgp_peer_state_t state; /**< 本 AF 下的协商状态 */
    uint32_t flags;         /**< per-peer 配置位（BGP_PEER_FLAG_*） */
    bgp_vrf_t *vrf;         /**< 所属 VRF（借用引用，不持有所有权） */
    bgp_instance_t *inst;   /**< 所属 AF 实例（借用引用，不持有所有权） */
    GList *subgroups;       /**< bgp_nh_subgroup_t* 借用引用列表（可同时归属多个子组） */
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
