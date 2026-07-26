/**
 * @file   isis_nexthop.h
 * @brief  ISIS protocol nexthop registry.
 */
#ifndef ISIS_NEXTHOP_H
#define ISIS_NEXTHOP_H

#include <stdint.h>

#include "net_addr.h"
#include "route_nhobj.h"

typedef struct isis_nexthop_table isis_nexthop_table_t;

typedef struct isis_nexthop_value
{
    net_addr_t relay_addr; /**< ROUTE/FIB 使用的 relay/网关地址 */
    uint32_t out_ifindex;  /**< ROUTE/FIB 使用的出接口 */
    uint32_t _pad0;        /**< 对齐填充 */
} isis_nexthop_value_t;

isis_nexthop_table_t *isis_nexthop_table_create(void);
void isis_nexthop_table_destroy(isis_nexthop_table_t *table);

int isis_nexthop_acquire(isis_nexthop_table_t *table, const route_nhobj_key_t *key, const isis_nexthop_value_t *value,
                         uint32_t *id_out);
int isis_nexthop_retain(isis_nexthop_table_t *table, uint32_t id);
void isis_nexthop_release(isis_nexthop_table_t *table, uint32_t id);

int isis_nexthop_lookup(isis_nexthop_table_t *table, uint32_t id, route_nhobj_key_t *key_out);
int isis_nexthop_get_value(isis_nexthop_table_t *table, uint32_t id, isis_nexthop_value_t *value_out);
int isis_nexthop_key_equal(const route_nhobj_key_t *a, const route_nhobj_key_t *b);

void isis_nexthop_make_route_key(uint32_t vrf_id, uint16_t afi, uint32_t key_ifindex, const net_addr_t *nexthop,
                                 route_nhobj_key_t *key);

/**
 * @brief ROUTE 进程重启后，把本表所有 nexthop 对象按原 id 反刷给 ROUTE（重建对象）
 * @param table nexthop 表
 * @return 成功反刷的对象数量
 */
uint32_t isis_nexthop_table_resync(isis_nexthop_table_t *table);

#endif /* ISIS_NEXTHOP_H */
