/**
 * @file   route_nhobj.h
 * @brief  Route nexthop 对象分配器：对标 tunnel-id，承载“下一跳描述”并按需下刷 FIB
 * @author jhb
 * @date   2026/06/03
 *
 * 设计：每个唯一的“原始下一跳标识”（vrf_id, protocol, afi, nh_type, nexthop, key_ifindex）
 * 对应一个 nexthop 对象。对象内嵌身份键 + relay 解析结果（relay_addr / relay 出接口）。
 *
 * 两类引用，互不相同：
 *   - registry 引用：每条 route_path 持有一份（acquire/release），决定对象在 ROUTE 内的生命周期；
 *     show/pub/relay 通过 route_nhobj_lookup(id) 读取下一跳/relay 信息。
 *   - FIB 引用：仅“已下 OS 的最优路由”持有（fib_attach/fib_detach）。首个 attach 下刷
 *     FIB_MSG_TYPE_NEXTHOP_UPSERT，末个 detach 下刷 FIB_MSG_TYPE_NEXTHOP_DELETE；FIB 在对象
 *     就绪前不会把引用它的路由下 OS。relay 变化时若处于 attach 态则重新 upsert（id 不变）。
 *
 * route_path 仅保存 nexthop_id（习惯用法），nexthop/relay 等不再各自冗余存一份。
 * 全部接口仅由 route worker 线程调用（无锁）。
 */
#ifndef ROUTE_NHOBJ_H
#define ROUTE_NHOBJ_H

#include <stdint.h>

#include "route.h"

/** nexthop 对象只读快照（供 show/pub/relay 读取） */
typedef struct route_nhobj_info
{
    route_nhobj_key_t key;  /**< 身份键（含原始 nexthop / key_ifindex） */
    net_addr_t relay_addr;  /**< relay 解析后的网关 */
    uint32_t relay_ifindex; /**< relay 解析后的出接口 */
} route_nhobj_info_t;

/**
 * @brief 初始化 nexthop 对象分配器（worker 线程内调用一次）
 */
void route_nhobj_init(void);

/**
 * @brief 清理 nexthop 对象分配器，释放所有表项（不再下发 FIB delete）
 */
void route_nhobj_cleanup(void);

/**
 * @brief 申请一个 nexthop 对象的 registry 引用（命中递增引用计数，否则分配新 id）
 *
 * 仅维护 ROUTE 内对象生命周期，不触发 FIB 下刷。relay 初值为空，后续由 route_nhobj_set_relay 更新。
 * id 由 ROUTE 在 key->protocol 的分区内分配；want_id 非 0 时（业务进程重启反刷）按该 id 恢复。
 *
 * @param key     nexthop 对象键（原始下一跳身份）
 * @param want_id 期望 id：0=ROUTE 分配；非 0=按该 id 恢复（重启反刷，id 落在协议分区内）
 * @param id_out  输出分配到的 nexthop_id（非 0）
 * @return 0 成功，-1 失败
 */
int route_nhobj_acquire(const route_nhobj_key_t *key, uint32_t want_id, uint32_t *id_out);

/**
 * @brief 按 id 增加一份 registry 引用
 * @param id nexthop 对象 id
 * @return 0 成功，-1 失败
 */
int route_nhobj_retain(uint32_t id);

/**
 * @brief 释放一个 registry 引用（引用归零则删除表项；若仍处于 FIB attach 态会先 detach）
 * @param id route_nhobj_acquire 返回的 id（0 忽略）
 */
void route_nhobj_release(uint32_t id);

/**
 * @brief 更新对象的 relay 解析结果；若对象处于 FIB attach 态则重新下刷 FIB（id 不变）
 * @param id            对象 id（0 忽略）
 * @param relay_addr    relay 网关（NULL/空表示无）
 * @param relay_ifindex relay 出接口（0=未知）
 */
void route_nhobj_set_relay(uint32_t id, const net_addr_t *relay_addr, uint32_t relay_ifindex);

/**
 * @brief 读取对象快照
 * @param id  对象 id
 * @param out 输出快照
 * @return 0 命中，-1 未找到
 */
int route_nhobj_lookup(uint32_t id, route_nhobj_info_t *out);

/**
 * @brief 比较两个 nexthop 对象键是否相等
 * @return 非 0 表示相等
 */
int route_nhobj_key_equal(const route_nhobj_key_t *a, const route_nhobj_key_t *b);

/**
 * @brief nexthop 对象遍历回调
 * @param nexthop_id   对象 id
 * @param info         对象快照（key + relay）
 * @param refcount     registry 引用计数（route_path 持有）
 * @param fib_refcount FIB 引用计数（>0 表示已下刷 FIB）
 * @param user         用户数据
 */
typedef void (*route_nhobj_iter_fn)(uint32_t nexthop_id, const route_nhobj_info_t *info, uint32_t refcount,
                                    uint32_t fib_refcount, void *user);

/**
 * @brief 遍历所有 nexthop 对象（供 show 命令）
 */
void route_nhobj_foreach(route_nhobj_iter_fn fn, void *user);

/**
 * @brief 增加一份 FIB 引用（首个引用下刷 nexthop 对象到 FIB）
 * @param id 对象 id（0 忽略）
 */
void route_nhobj_fib_attach(uint32_t id);

/**
 * @brief 减少一份 FIB 引用（末个引用从 FIB 撤销 nexthop 对象）
 * @param id 对象 id（0 忽略）
 */
void route_nhobj_fib_detach(uint32_t id);

#endif /* ROUTE_NHOBJ_H */
