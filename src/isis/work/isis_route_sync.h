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
void isis_route_sync_withdraw_all_instance_routes(isis_instance_cfg_t *inst);

const char *isis_route_sync_if_event_get_logical_name(const dev_ipc_message_t *msg, char *ifname_out, size_t out_sz);

#endif /* ISIS_ROUTE_SYNC_H */
