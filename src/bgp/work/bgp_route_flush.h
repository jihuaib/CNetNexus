/**
 * @file   bgp_route_flush.h
 * @brief  BGP → ROUTE 模块路由下刷队列与下刷工作事件
 * @author jhb
 * @date   2026/04/17
 *
 * best-path 完成后将对应 rthead 推入下刷队列，worker 事件循环批量出队：
 *   - 撤销已下刷但不再是 best+valid 的路径
 *   - 下刷当前 best+valid 路由到 ROUTE 模块
 */
#ifndef BGP_ROUTE_FLUSH_H
#define BGP_ROUTE_FLUSH_H

#include <glib.h>
#include <stdint.h>

#include "bgp.h"
#include "bgp_instance.h"
#include "bgp_rib.h"

typedef struct bgp_rthead bgp_rthead_t;

// ============================================================================
// 路由下刷队列
// ============================================================================

/**
 * @brief 向 ROUTE 模块下刷的工作队列
 *
 * 队列元素为 bgp_rthead_t*（入队加引用，出队减引用）。
 * 路由是否已下刷通过 bgp_route_node_t.flags 的 BGP_ROUTE_FLAG_FLUSHED 维护。
 */
typedef struct bgp_route_flush_queue
{
    GQueue *q;                  /**< FIFO 队列（元素为 bgp_rthead_t*） */
    uint32_t count;             /**< 当前队列中的条目数 */
    gboolean defer_until_ready; /**< ROUTE IPC 失败后等 READY 重放，避免自旋 */
} bgp_route_flush_queue_t;

bgp_route_flush_queue_t *bgp_route_flush_queue_create(void);

void bgp_route_flush_queue_destroy(bgp_route_flush_queue_t *q, bgp_instance_t *inst);

/**
 * @brief 将 rthead 推入 ROUTE 下刷队列（入队时加引用）
 * @return 0 成功，-1 参数无效
 */
int bgp_route_flush_queue_push(bgp_route_flush_queue_t *q, bgp_rthead_t *head);

/**
 * @brief 批量处理 ROUTE 下刷队列
 * @return 实际处理条目数
 */
int bgp_route_flush_queue_process(bgp_route_flush_queue_t *q, bgp_instance_t *inst, int batch_size);

/**
 * @brief 处理一条 BGP route-flush 工作事件（worker 线程调用）
 */
void bgp_route_flush_handle_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 在当前线程同步抽干 inst 的下刷队列（不允许重新调度事件）
 * @return 实际处理条目数
 */
int bgp_route_flush_process_pending(bgp_instance_t *inst);

/**
 * @brief 销毁实例前的 ROUTE 同步撤销屏障。
 *
 * 遍历实例所有 RIB，对带 BGP_ROUTE_FLAG_FLUSHED 的路径调用
 * route_rpc_del_wait()；只有收到 ROUTE 成功 ACK 才清除已下刷标记。
 * 可幂等重试：部分成功后遇到失败时，未确认条目保留标记。
 *
 * @return ERRCODE_SUCCESS=已确认撤销所有已下刷路径；ERRCODE_FAIL=不允许销毁实例
 */
int bgp_route_flush_withdraw_instance_sync(bgp_instance_t *inst);

/**
 * @brief ROUTE READY/restart 后，将已标记 FLUSHED 的 best 路由清标并重新入队下刷。
 * @return 入队的前缀头数量
 */
uint32_t bgp_route_flush_replay_flushed_all(void);

/**
 * @brief BGP 进程优雅退出前,把已下刷给 ROUTE 的所有路由都主动撤销。
 *
 * 遍历 protocol->vrf_hash → inst_hash → rd_entries → rib.head_tree → head.route_list,
 * 对带 BGP_ROUTE_FLAG_FLUSHED 标记的 route 直接 route_rpc_del 给 ROUTE。
 * 避免依赖关闭路径上的 route_flush_queue/calc_queue（关闭时这两个队列只销毁不处理）。
 *
 * 仅在 bgp_worker_runtime_cleanup 调用,IPC ctx 此时尚未销毁。
 *
 * @return 实际撤销的路由条目数
 */
uint32_t bgp_route_flush_withdraw_all_for_shutdown(void);

#endif /* BGP_ROUTE_FLUSH_H */
