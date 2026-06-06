/**
 * @file   route_db.h
 * @brief  Route 模块 DB 操作接口（在 IPC 线程调用）
 * @author jhb
 * @date   2026/03/28
 */
#ifndef ROUTE_DB_H
#define ROUTE_DB_H

#include <stdint.h>

#include "dev.h"

/**
 * @brief 插入或更新一条静态路由记录
 *
 * @param ctx         IPC 上下文
 * @param vrf_name    VRF 名称（NULL/空字符串视为公网 "public"）
 * @param afi         地址族
 * @param prefix_str  前缀地址字符串
 * @param prefix_len  前缀长度
 * @param nexthop_str 下一跳地址字符串（interface-only 时传空字符串）
 * @param metric      度量值
 * @param preference  管理距离
 * @param ifname      出接口逻辑名（不约束时传空字符串）
 */
void route_db_upsert_static(dev_ipc_context_t *ctx, const char *vrf_name, uint16_t afi, const char *prefix_str,
                            uint8_t prefix_len, const char *nexthop_str, int32_t metric, int32_t preference,
                            const char *ifname);

/**
 * @brief 删除一条静态路由记录（精确匹配 nexthop + ifname）
 *
 * @param ctx         IPC 上下文
 * @param vrf_name    VRF 名称（NULL/空字符串视为公网 "public"）
 * @param afi         地址族
 * @param prefix_str  前缀地址字符串
 * @param prefix_len  前缀长度
 * @param nexthop_str 下一跳地址字符串（interface-only 时传空字符串）
 * @param ifname      出接口逻辑名（不约束时传空字符串）
 */
void route_db_delete_static(dev_ipc_context_t *ctx, const char *vrf_name, uint16_t afi, const char *prefix_str,
                            uint8_t prefix_len, const char *nexthop_str, const char *ifname);

/**
 * @brief 删除某前缀下所有静态路由记录
 *
 * @param ctx        IPC 上下文
 * @param vrf_name   VRF 名称（NULL/空字符串视为公网 "public"）
 * @param afi        地址族
 * @param prefix_str 前缀地址字符串
 * @param prefix_len 前缀长度
 */
void route_db_delete_static_prefix(dev_ipc_context_t *ctx, const char *vrf_name, uint16_t afi, const char *prefix_str,
                                   uint8_t prefix_len);

/**
 * @brief 删除指定 VRF 下所有静态路由记录（用于 VRF 删除级联）
 *
 * @param ctx        IPC 上下文
 * @param vrf_name   VRF 名称
 * @return 实际删除的记录数（>=0），失败返回 -1
 */
int route_db_delete_static_by_vrf(dev_ipc_context_t *ctx, const char *vrf_name);

/**
 * @brief 插入或更新 batch 路由记录（name 为主键）
 *
 * @param ctx        IPC 上下文
 * @param name       batch 名称（主键）
 * @param vrf_name   VRF 名称（NULL/空字符串视为公网 "public"）
 * @param afi        地址族
 * @param start_addr 起始地址字符串
 * @param prefix_len 前缀长度
 * @param count      路由数量
 * @param nexthop    下一跳地址字符串（interface-only 时传空字符串）
 * @param metric     度量值
 * @param preference 管理距离
 * @param ifname     出接口逻辑名（不约束时传空字符串）
 */
void route_db_upsert_batch(dev_ipc_context_t *ctx, const char *name, const char *vrf_name, uint16_t afi,
                           const char *start_addr, uint8_t prefix_len, int64_t count, const char *nexthop,
                           int32_t metric, int32_t preference, const char *ifname);

/**
 * @brief 删除 batch 路由记录
 *
 * @param ctx  IPC 上下文
 * @param name batch 名称
 */
void route_db_delete_batch(dev_ipc_context_t *ctx, const char *name);

/**
 * @brief 从 DB 恢复 route_static / route_batch 到内存态（通过 worker 同步派发 apply）
 *
 * 该函数在 IPC 线程调用，但不会直接操作 worker 内部数据结构；
 * 所有内存态变更均通过 route_worker_dispatch_apply() 在 worker 线程执行。
 *
 * @return ERRCODE_SUCCESS 成功；ERRCODE_FAIL 失败
 */
int route_db_restore(void);

/**
 * @brief VRF 重启 re-sync：只重恢复 vrf_name 非 public 的 route_static 行。
 */
int route_db_restore_vrf_bound(void);

#endif /* ROUTE_DB_H */
