/**
 * @file   isis_route_sync.h
 * @brief  ISIS 路由学习/撤销与接口事件重算
 * @author jhb
 * @date   2026/04/12
 */
#ifndef ISIS_ROUTE_SYNC_H
#define ISIS_ROUTE_SYNC_H

#include <stddef.h>

#include "isis_worker.h"

int isis_route_sync_publish_add(const isis_route_state_t *state);
int isis_route_sync_publish_del(const isis_route_state_t *state);

void isis_route_sync_reconcile_instance_if(isis_instance_cfg_t *inst, const char *ifname);
void isis_route_sync_reconcile_instance_all_if(isis_instance_cfg_t *inst);
void isis_route_sync_reconcile_all_instances_if(const char *ifname);
void isis_route_sync_reconcile_all_instances(void);
void isis_route_sync_replay_all_instances(void);
void isis_route_sync_withdraw_all_instance_routes(isis_instance_cfg_t *inst);

/**
 * @brief ROUTE 进程重启后，把所有 instance 的 nexthop 对象按原 id 反刷给 ROUTE（重建对象）
 *
 * 须在 reconcile/replay 之前调用：先恢复 nexthop 对象，后续 replay 走 by_key 命中即复用同一 id。
 */
void isis_nexthop_resync_all_instances(void);

const char *isis_route_sync_if_event_get_logical_name(const dev_ipc_message_t *msg, char *ifname_out, size_t out_sz);

#endif /* ISIS_ROUTE_SYNC_H */
