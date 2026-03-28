/**
 * @file   bgp_calc.h
 * @brief  BGP 路由优选接口（best-path 计算）
 * @author jhb
 * @date   2026/03/15
 */
#ifndef BGP_CALC_H
#define BGP_CALC_H

#include <stdint.h>

/* 包含顺序：bgp_instance.h 传递包含 bgp_peer.h（定义枚举），必须先于 bgp.h（定义同名宏） */
#include "bgp.h"
#include "bgp_instance.h"

// ============================================================================
// 路由优选入口
// ============================================================================

/**
 * @brief 触发指定实例的全量路由优选计算（占位）
 *
 * 遍历 inst->rib，对每个 NLRI 执行最优路径选择，通过 bgp_rib_mark_best()
 * 在路由节点上打标记。当前为占位实现。
 *
 * @param inst 目标地址族实例
 * @return 0 成功，-1 失败
 */
int bgp_calc_run(bgp_instance_t *inst);

/**
 * @brief 对单条 NLRI 执行 best-path 计算（由工作队列定时调用）
 *
 * 通过 NLRI 在 inst->rib 中定位 rthead，选出最优路径：
 *   - rthead 不存在或路径哈希为空：同步发送 WITHDRAW（调用
 *     bgp_work_send_withdraw_to_all）
 *   - 有路由：选出最优路径，通过 bgp_rib_mark_best() 在路由节点上打标记，
 *     推 ANNOUNCE 到 pub_queue，并将 NLRI 推入 route_flush_queue 下刷 ROUTE
 *
 * @param inst 目标地址族实例
 * @param nlri NLRI 条目（由 calc_queue 条目提供）
 */
void bgp_calc_run_one(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

#endif /* BGP_CALC_H */
