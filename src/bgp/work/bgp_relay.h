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
 */
void bgp_relay_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd,
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

#endif /* BGP_RELAY_H */
