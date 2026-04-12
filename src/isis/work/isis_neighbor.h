/**
 * @file   isis_neighbor.h
 * @brief  ISIS 邻居发现与老化（LAN IIH）
 * @author jhb
 * @date   2026/04/12
 */
#ifndef ISIS_NEIGHBOR_H
#define ISIS_NEIGHBOR_H

#include "isis_worker.h"

int isis_neighbor_prepare(int epoll_fd, void *raw_tag, void *tick_tag);
void isis_neighbor_shutdown(int epoll_fd);

void isis_neighbor_handle_raw_event(void);
void isis_neighbor_handle_tick_event(void);

void isis_neighbor_on_if_removed(isis_instance_cfg_t *inst, const char *ifname);
void isis_neighbor_reconcile_instance(isis_instance_cfg_t *inst);
void isis_neighbor_reconcile_if(const char *ifname);
void isis_neighbor_reconcile_all(void);

#endif /* ISIS_NEIGHBOR_H */
