/**
 * @file   bgp_relay.h
 * @brief  BGP nexthop relay：维护 nexthop 与路由关系，并消费 ROUTE nexthop 回调
 */
#ifndef BGP_RELAY_H
#define BGP_RELAY_H

#include <stdint.h>

#include "bgp.h"
#include "net_addr.h"
#include "route.h"
#include "tunnel.h"

typedef struct bgp_session bgp_session_t;
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
 * @brief 清理指定 source 的所有 relay 路由
 */
void bgp_relay_flush_peer_routes(uint32_t vrf_id, const net_addr_t *source);

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
