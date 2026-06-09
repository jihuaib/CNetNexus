/**
 * @file   bgp_relay.h
 * @brief  BGP nexthop relay：维护 nexthop 与路由关系，并消费 ROUTE nexthop 回调
 */
#ifndef BGP_RELAY_H
#define BGP_RELAY_H

#include <stdint.h>

#include "bgp.h"
#include "bgp_nexthop.h"
#include "net_addr.h"
#include "route.h"
#include "tunnel.h"

typedef struct bgp_session bgp_session_t;
typedef struct bgp_instance bgp_instance_t;
typedef struct bgp_peer_update_ingest_stats bgp_peer_update_ingest_stats_t;

/**
 * @brief 初始化 relay 内部状态
 */
void bgp_relay_init(void);

/**
 * @brief 清理 relay 内部状态
 */
void bgp_relay_cleanup(void);

/**
 * @brief 处理对端 UPDATE：维护 route<->nexthop 关系并向 ROUTE 注册/撤销 nexthop watch
 *
 * @param base_attr 借用的对端原始属性；upd->attr 为当前实例生效属性时用于保存 RIB base_attr
 */
void bgp_relay_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd, const bgp_attr_t *base_attr,
                                  bgp_peer_update_ingest_stats_t *stats);

/**
 * @brief VRF export-RT 变化后，重建该 VRF unicast RIB 已有路由的 effective attr
 *
 * peer 路由从 route->base_attr（对端原始属性）重新合当前 VRF export-RT；
 * import-route 路由从默认 imported 属性重新合当前 VRF export-RT。
 * 正常路径异步分片处理并返回 0；仅投递失败的同步兜底路径返回实际 changed head 数。
 */
uint32_t bgp_relay_vrf_export_attr_rebuild(uint32_t vrf_id, bgp_afi_t afi);

/**
 * @brief 清理指定 source 的所有 relay 路由
 */
void bgp_relay_flush_peer_routes(uint32_t vrf_id, const net_addr_t *source);

/**
 * @brief 实例销毁前清理该实例所有 route 持有的 relay nexthop watch 借用
 *
 * instance/RIB teardown 不走 peer withdraw 路径；若不先解除 watch 对 route 的 borrow，
 * RIB destroy 会因 borrow_refcnt > 0 跳过 route_node_free，造成 route 泄漏。
 */
void bgp_relay_flush_instance_routes(bgp_instance_t *inst);

/**
 * @brief 读取 route 当前 nexthop 迭代 value。
 *
 * 普通 IP nexthop 从 BGP nexthop registry 读取；labeled/import-rib 隧道路径从 relay watch 读取。
 */
int bgp_relay_get_route_iter_value(const bgp_route_node_t *route, bgp_nexthop_value_t *value_out);

/**
 * @brief 为合成路由（vrf-import REMOTE_CROSS）注册自有 nexthop 迭代 watch
 *
 * 合成路由不走 peer-update 摄取路径，需调用方在创建/刷新时显式注册，使其下一跳（远端 PE）
 * 在公网表做隧道迭代命中 eBGP-vpnv4 假隧道。
 * @return 该 nexthop 当前是否已解析（resolved），调用方据此置路由 valid
 */
gboolean bgp_relay_synthetic_nexthop_register(bgp_route_node_t *route);

/**
 * @brief 注销合成路由的 nexthop 迭代 watch（撤销/拆除时调用）
 */
void bgp_relay_synthetic_nexthop_unregister(bgp_route_node_t *route);

/**
 * @brief 处理 ROUTE nexthop 通知（可达/不可达）
 * @return 受影响路由数
 */
uint32_t bgp_relay_handle_nh_notify(const route_nh_iter_notify_t *notify);

/**
 * @brief ROUTE READY/restart 后重发当前所有 ROUTE nexthop 迭代注册。
 *
 * labeled-unicast watch 属于 TUNNEL，不在此函数内重注册。
 */
uint32_t bgp_relay_reregister_route_nexthops(void);

/**
 * @brief 处理 TUNNEL 解析通知（LU 隧道可达/不可达）
 * @return 受影响路由数
 */
uint32_t bgp_relay_handle_tunnel_notify(const tunnel_resolve_notify_t *notify);

/**
 * @brief BGP-LU session adjacency candidate 同步
 */
void bgp_relay_session_lu_adj_sync(bgp_session_t *session, gboolean up);

#endif /* BGP_RELAY_H */
