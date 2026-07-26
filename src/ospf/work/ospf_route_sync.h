/**
 * @file   ospf_route_sync.h
 * @brief  Asynchronous OSPF route synchronization
 */
#ifndef OSPF_ROUTE_SYNC_H
#define OSPF_ROUTE_SYNC_H

typedef struct ospf_route ospf_route_t;

int ospf_route_sync_prepare(void);
int ospf_route_sync_launch(void);
void ospf_route_sync_shutdown(void);

int ospf_route_sync_enqueue_add(const ospf_route_t *route);
int ospf_route_sync_enqueue_del(const ospf_route_t *route);
int ospf_route_sync_enqueue_replace(const ospf_route_t *current, const ospf_route_t *desired);

#endif /* OSPF_ROUTE_SYNC_H */
