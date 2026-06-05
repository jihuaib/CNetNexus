/**
 * @file   isis_route.h
 * @brief  ISIS SPF 路由多路径存储（head + path）与最优路径同步
 * @author jhb
 * @date   2026/04/14
 */
#ifndef ISIS_ROUTE_H
#define ISIS_ROUTE_H

#include <glib.h>
#include <stddef.h>

#include "isis_worker.h"

typedef struct isis_route_path
{
    struct isis_route_head *head; /**< 所属 head（借用引用） */
    char *path_key;               /**< path 唯一键（拥有） */
    isis_route_state_t state;     /**< 路径状态 */
} isis_route_path_t;

typedef struct isis_route_head
{
    char *route_key;  /**< route 唯一键（拥有） */
    GList *path_list; /**< isis_route_path_t*，按优先级升序，首元素为最优 */
    uint32_t path_count;
} isis_route_head_t;

/** 创建 head 哈希表：key=route_key(strdup), value=isis_route_head_t* */
GHashTable *isis_route_head_table_new(void);

/** 销毁单个 head（可作为 GHashTable value destroy func） */
void isis_route_head_free(gpointer data);

/** route_state 精确比较 */
int isis_route_state_same(const isis_route_state_t *a, const isis_route_state_t *b);

/** 释放 route_state 持有的 nexthop 引用并清零 id */
void isis_route_state_reset(isis_route_state_t *state);

/** 深拷贝 route_state，并为 nexthop_id 增加引用 */
int isis_route_state_copy(isis_route_state_t *dst, const isis_route_state_t *src);

/** 按 ROUTE nexthop key 规则设置 nexthop 引用；key_ifindex 参与身份，out_ifindex 写入 value */
int isis_route_state_set_nexthop(isis_route_state_t *state, isis_nexthop_table_t *table, uint32_t key_ifindex,
                                 uint32_t out_ifindex, const net_addr_t *nexthop);

/** 读取 route_state 的 nexthop 地址 */
int isis_route_state_get_nexthop(const isis_route_state_t *state, net_addr_t *nexthop_out);

/** 读取 route_state 的 nexthop value 出接口 */
int isis_route_state_get_out_ifindex(const isis_route_state_t *state, uint32_t *out_ifindex);

/**
 * 生成路径键：<route_key>|oif=<ifindex>|nh=<addr>|src=<addr>
 */
void isis_route_path_key_format(char *buf, size_t sz, const char *route_key, const isis_route_state_t *state);

/** 向 head 表写入/更新一条路径（自动排序） */
void isis_route_head_table_add_path(GHashTable *head_table, const char *route_key, const char *path_key,
                                    const isis_route_state_t *state);

/** 获取 head 当前最优路径（path_list 首元素） */
const isis_route_path_t *isis_route_head_best_path(const isis_route_head_t *head);

/**
 * SPF 路由对账：
 * - 以 desired_heads 计算每个 route_key 的 best-path；
 * - 与 inst->learned_route_heads 的当前 best 做增删下发；
 * - 仅刷新 inst->learned_route_heads 中 lsp|* 路由（保留 host|*）。
 */
int isis_route_reconcile_spf(isis_instance_cfg_t *inst, const GHashTable *desired_heads);

#endif /* ISIS_ROUTE_H */
