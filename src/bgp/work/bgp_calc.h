/**
 * @file   bgp_calc.h
 * @brief  BGP 路由优选接口（best-path 计算）
 * @author jhb
 * @date   2026/03/15
 */
#ifndef BGP_CALC_H
#define BGP_CALC_H

#include <glib.h>
#include <stdint.h>

/* 包含顺序：bgp_instance.h 传递包含 bgp_peer.h（定义枚举），必须先于 bgp.h（定义同名宏） */
#include "bgp.h"
#include "bgp_instance.h"
#include "bgp_rib.h"

typedef struct bgp_rthead bgp_rthead_t;

// ============================================================================
// 优选工作队列（calc_queue）
// ============================================================================

/**
 * @brief 优选工作队列
 *
 * FIFO 队列，路由变化时入队，工作事件批量出队处理。
 * GQueue 元素为 bgp_rthead_t*（入队加引用，出队减引用）。
 */
typedef struct bgp_calc_queue
{
    GQueue *q;      /**< FIFO 队列（元素为 bgp_rthead_t*） */
    uint32_t count; /**< 当前队列中的条目数 */
} bgp_calc_queue_t;

/**
 * @brief 创建优选工作队列
 */
bgp_calc_queue_t *bgp_calc_queue_create(void);

/**
 * @brief 销毁优选工作队列（释放所有未处理条目）
 * @param q    队列（允许为 NULL）
 * @param inst 所属实例（用于释放 rthead 引用，可为 NULL）
 */
void bgp_calc_queue_destroy(bgp_calc_queue_t *q, bgp_instance_t *inst);

/**
 * @brief 将 NLRI 推入优选队列
 * @return 0 成功，-1 参数无效
 */
int bgp_calc_queue_push(bgp_calc_queue_t *q, bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 批量处理优选队列（每次处理至多 batch_size 条）
 * @return 实际处理条目数
 */
int bgp_calc_queue_process(bgp_calc_queue_t *q, bgp_instance_t *inst, int batch_size);

/**
 * @brief 处理一条 BGP calc 工作事件（worker 线程调用）
 */
void bgp_calc_handle_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 在当前线程同步抽干 inst 的 calc_queue（不允许重新调度事件）
 * @return 实际处理条目数
 */
int bgp_calc_process_pending(bgp_instance_t *inst);

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
 *   - rthead 不存在或路径哈希为空：挂入各 subgroup 的 withdraw_queue
 *     （bgp_update_group_enqueue_withdraw）
 *   - 有路由：选出最优路径，通过 bgp_rib_mark_best() 在路由节点上打标记，
 *     挂入各 subgroup 的 announce_queue，并将 NLRI 推入 route_flush_queue 下刷 ROUTE
 *
 * @param inst 目标地址族实例
 * @param nlri NLRI 条目（由 calc_queue 条目提供）
 */
void bgp_calc_run_one(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

#endif /* BGP_CALC_H */
