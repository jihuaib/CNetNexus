/**
 * @file   bgp_nexthop.h
 * @brief  BGP nexthop object registry.
 */
#ifndef BGP_NEXTHOP_H
#define BGP_NEXTHOP_H

#include <stdint.h>

#include "bgp_rib.h"
#include "route.h"

/* 前向声明：避免引入 bgp_protocol.h */
typedef struct bgp_protocol bgp_protocol_t;

typedef struct bgp_nexthop_value
{
    net_addr_t iter_relay_addr; /**< nexthop 迭代得到的 relay 地址（family=0 表示未知） */
    gint64 updated_at_usec;     /**< 最近一次 value 更新时间 */
    uint32_t iter_out_ifindex;  /**< nexthop 迭代得到的出接口索引（0 表示未知） */
    uint32_t tunnel_id;         /**< nexthop 迭代得到的隧道 ID（0 表示未使用隧道） */
    uint8_t iter_watched;       /**< 是否已挂 relay watch（1=是，0=否） */
    uint8_t iter_resolved;      /**< nexthop 迭代是否可达（1=可达，0=不可达） */
    uint8_t _pad0[2];           /**< 对齐填充 */
} bgp_nexthop_value_t;

void bgp_nexthop_init(bgp_instance_t *inst);
void bgp_nexthop_cleanup(bgp_instance_t *inst);

void bgp_nexthop_make_route_key(const bgp_route_node_t *route, const net_addr_t *nexthop, route_nhobj_key_t *key);

int bgp_nexthop_acquire(bgp_instance_t *inst, const route_nhobj_key_t *key, uint32_t *id_out);
int bgp_nexthop_retain(bgp_instance_t *inst, uint32_t id);
void bgp_nexthop_release(bgp_instance_t *inst, uint32_t id);
int bgp_nexthop_lookup(bgp_instance_t *inst, uint32_t id, route_nhobj_key_t *key_out);
int bgp_nexthop_get_value(bgp_instance_t *inst, uint32_t id, bgp_nexthop_value_t *value_out);
int bgp_nexthop_set_value(bgp_instance_t *inst, uint32_t id, const bgp_nexthop_value_t *value);
void bgp_nexthop_clear_value(bgp_instance_t *inst, uint32_t id);
int bgp_nexthop_key_equal(const route_nhobj_key_t *a, const route_nhobj_key_t *b);

int bgp_nexthop_get_route_key(const bgp_route_node_t *route, route_nhobj_key_t *key_out);
int bgp_nexthop_get_route_addr(const bgp_route_node_t *route, net_addr_t *nexthop_out);
int bgp_nexthop_get_route_bgp(const bgp_route_node_t *route, bgp_nexthop_t *nexthop_out);
int bgp_nexthop_set_route_key(bgp_route_node_t *route, const route_nhobj_key_t *key);
int bgp_nexthop_set_route(bgp_route_node_t *route, const bgp_nexthop_t *nexthop);
void bgp_nexthop_reset_route(bgp_route_node_t *route);

/**
 * @brief ROUTE 进程重启后，把本模块所有 nexthop 对象按原 id 反刷给 ROUTE
 *
 * id 由 BGP 在协议分区内分配且永不改变，重启后用同一 id 重发 acquire 让 ROUTE 重建对象；
 * route->nexthop_id 与 relay watch 因而无需改动。需在 ROUTE READY 时、重注册 watch / 重下刷
 * 路由之前调用。
 *
 * @param proto BGP 协议实例（遍历其 vrf/instance 下的 nexthop 表）
 * @return 成功反刷的对象数量
 */
uint32_t bgp_nexthop_resync_all(bgp_protocol_t *proto);

#endif /* BGP_NEXTHOP_H */
