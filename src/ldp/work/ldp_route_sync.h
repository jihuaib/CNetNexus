/**
 * @file   ldp_route_sync.h
 * @brief  与 ROUTE 模块的同步：订阅、路由学习、TUNNEL candidate 注册
 * @author jhb
 * @date   2026/05/06
 */
#ifndef LDP_ROUTE_SYNC_H
#define LDP_ROUTE_SYNC_H

#include "ldp_lib.h"
#include "ldp_worker.h"

/** LDP 在 TUNNEL 中的候选优先级（越小越优先；BGP-LU=100，给 LDP 略低） */
#define LDP_TUNNEL_PREFERENCE 90u

/** LDP 一条已学到的 IPv4 unicast 路由（仅记录与 LSP 相关字段） */
typedef struct ldp_learned_route
{
    ldp_fec_t fec;
    uint32_t vrf_id;
    uint32_t nexthop_v4; /**< host order；0 表示未知 */
    uint32_t out_ifindex;
    uint32_t local_label; /**< 0 表示尚未分配 */
} ldp_learned_route_t;

/* 生命周期 */
void ldp_route_sync_init(void);
void ldp_route_sync_subscribe(void);
void ldp_route_sync_resubscribe(void);
void ldp_route_sync_unsubscribe(void);
void ldp_route_sync_cleanup(void);

/* IPC 进来的路由事件（worker 线程内） */
void ldp_route_sync_on_route_msg(dev_ipc_message_t *msg);

/* session FSM 钩子 */
void ldp_route_sync_on_session_up(uint32_t peer_lsr_id, uint16_t peer_label_space);
void ldp_route_sync_on_session_down(uint32_t peer_lsr_id, uint16_t peer_label_space);
void ldp_route_sync_on_remote_label(uint32_t peer_lsr_id, uint16_t peer_label_space, const ldp_fec_t *fec,
                                    uint32_t label);
void ldp_route_sync_on_remote_label_withdraw(uint32_t peer_lsr_id, uint16_t peer_label_space, const ldp_fec_t *fec);

/* 表（show 用） */
GHashTable *ldp_route_sync_routes_table(void);

#endif /* LDP_ROUTE_SYNC_H */
