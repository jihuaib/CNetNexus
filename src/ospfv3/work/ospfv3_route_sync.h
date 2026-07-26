/**
 * @file   ospfv3_route_sync.h
 * @brief  Asynchronous OSPFV3 route synchronization
 */
#ifndef OSPFV3_ROUTE_SYNC_H
#define OSPFV3_ROUTE_SYNC_H

typedef struct ospfv3_route ospfv3_route_t;

int ospfv3_route_sync_prepare(void);
int ospfv3_route_sync_launch(void);
void ospfv3_route_sync_shutdown(void);

int ospfv3_route_sync_enqueue_add(const ospfv3_route_t *route);
int ospfv3_route_sync_enqueue_del(const ospfv3_route_t *route);
int ospfv3_route_sync_enqueue_replace(const ospfv3_route_t *current, const ospfv3_route_t *desired);

#endif /* OSPFV3_ROUTE_SYNC_H */
