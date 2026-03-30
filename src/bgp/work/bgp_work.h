/**
 * @file   bgp_work.h
 * @brief  BGP 路由工作处理
 * @author jhb
 * @date   2026/03/15
 */
#ifndef BGP_WORK_H
#define BGP_WORK_H

#include <glib.h>
#include <stdint.h>

/* 包含顺序：bgp_peer.h 定义 bgp_afi_t/bgp_safi_t 枚举，必须先于 bgp.h（定义同名宏） */
#include "bgp.h"
#include "bgp_peer.h"
#include "bgp_session.h"

/* bgp_instance.h 包含本头文件，用前向声明打破循环 */
typedef struct bgp_instance bgp_instance_t;
typedef struct bgp_rthead bgp_rthead_t;

/** 每次工作事件处理时每个数据队列处理的最大条目数 */
#define BGP_WORK_BATCH_SIZE 64

// ============================================================================
// 优选队列（calc_queue）
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

// ============================================================================
// 发布队列（pub_queue）
// ============================================================================

/**
 * @brief 发布工作队列
 *
 * FIFO 队列，best-path 完成后按 session 入队，工作事件批量出队向邻居发包。
 * 只处理 ANNOUNCE：处理时通过 NLRI 在对应 AF 的 RIB 中查找 is_best 路径信息。
 * WITHDRAW 由 bgp_calc_run_one() 同步调用 bgp_work_send_withdraw_to_all() 发出。
 * GQueue 元素为内部 pub-item（持有 NLRI 副本）。
 */
typedef struct bgp_pub_queue
{
    GQueue *q;      /**< FIFO 队列（元素为内部 pub-item） */
    uint32_t count; /**< 当前队列中的条目数 */
} bgp_pub_queue_t;

// ============================================================================
// 路由下刷队列（route_flush_queue）
// ============================================================================

/**
 * @brief 向 ROUTE 模块下刷的工作队列
 *
 * FIFO 队列，best-path 完成后入队，工作事件批量出队下刷到 ROUTE 模块。
 * 队列元素为 bgp_rthead_t*（入队加引用，出队减引用）。
 * 路由是否已下刷通过 bgp_route_node_t.flags 的 BGP_ROUTE_FLAG_FLUSHED 维护。
 */
typedef struct bgp_route_flush_queue
{
    GQueue *q;      /**< FIFO 队列（元素为 bgp_rthead_t*） */
    uint32_t count; /**< 当前队列中的条目数 */
} bgp_route_flush_queue_t;

// ============================================================================
// API
// ============================================================================

/* ---- 优选队列 ---- */

/**
 * @brief 创建优选工作队列
 * @return 新建的 bgp_calc_queue_t 指针
 */
bgp_calc_queue_t *bgp_calc_queue_create(void);

/**
 * @brief 销毁优选工作队列（释放所有未处理条目）
 * @param q bgp_calc_queue_t 指针（允许为 NULL）
 * @param inst 所属实例（用于释放 rthead 引用，可为 NULL）
 */
void bgp_calc_queue_destroy(bgp_calc_queue_t *q, bgp_instance_t *inst);

/**
 * @brief 将 NLRI 推入优选队列
 * @param q    优选队列
 * @param inst 所属实例（用于通过 NLRI 定位/创建 rthead）
 * @param nlri NLRI 条目（匹配键）
 * @return 0 成功，-1 参数无效
 */
int bgp_calc_queue_push(bgp_calc_queue_t *q, bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 批量处理优选队列（每次处理至多 batch_size 条）
 *
 * 从 calc_queue 出队 NLRI，调用 bgp_calc_run_one()；
 * best-path 变化后的发布与 ROUTE 下刷由后续数据队列继续承接。
 *
 * @param q          优选队列
 * @param inst       目标地址族实例
 * @param batch_size 本次处理上限
 * @return 实际处理条目数
 */
int bgp_calc_queue_process(bgp_calc_queue_t *q, bgp_instance_t *inst, int batch_size);

/* ---- 发布队列 ---- */

/**
 * @brief 创建发布工作队列
 * @return 新建的 bgp_pub_queue_t 指针
 */
bgp_pub_queue_t *bgp_pub_queue_create(void);

/**
 * @brief 销毁发布工作队列（释放所有未处理条目）
 * @param q bgp_pub_queue_t 指针（允许为 NULL）
 */
void bgp_pub_queue_destroy(bgp_pub_queue_t *q);

/**
 * @brief 清空发布工作队列
 * @param q bgp_pub_queue_t 指针（允许为 NULL）
 */
void bgp_pub_queue_clear(bgp_pub_queue_t *q);

/**
 * @brief 从发布队列中删除指定 AF 的所有待发条目
 * @param q    发布队列
 * @param afi  地址族
 * @param safi 子地址族
 */
void bgp_pub_queue_drop_instance(bgp_pub_queue_t *q, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 统计发布队列中属于指定 AF 的待发条目数
 * @param q    发布队列
 * @param inst 目标地址族实例
 * @return 匹配条目数
 */
uint32_t bgp_pub_queue_count_for_instance(const bgp_pub_queue_t *q, const bgp_instance_t *inst);

/**
 * @brief 将 NLRI 推入发布队列（仅 ANNOUNCE）
 *
 * pub_queue 存储 NLRI 副本；处理时再按 AF 在 RIB 中查找当前 best 路径。
 *
 * @param q    发布队列
 * @param nlri NLRI 条目
 * @return 0 成功，-1 参数无效
 */
int bgp_pub_queue_push(bgp_pub_queue_t *q, const bgp_nlri_entry_t *nlri);

/**
 * @brief 批量处理发布队列（每次处理至多 batch_size 条）
 *
 * 从 pub_queue 中筛出属于 inst 的任务，向该 session 发送 UPDATE（ANNOUNCE）。
 * 仅在 session 处于 ESTABLISHED 时处理；否则保留队列，等待重建后再发。
 *
 * @param q          发布队列
 * @param sess       目标邻居会话
 * @param inst       目标地址族实例
 * @param batch_size 本次处理上限
 * @return 实际处理条目数
 */
int bgp_pub_queue_process(bgp_pub_queue_t *q, bgp_session_t *sess, bgp_instance_t *inst, int batch_size);

/**
 * @brief 将一个 best-route 变更挂入该实例所有 ESTABLISHED 邻居的 session pub_queue
 * @param inst 目标地址族实例
 * @param nlri 待发布 NLRI
 */
void bgp_work_enqueue_announce_to_established(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 邻居进入 ESTABLISHED 后，将其当前所有 best-route 快照挂入 session pub_queue
 * @param sess 目标会话
 */
void bgp_work_enqueue_best_for_session(bgp_session_t *sess);

/* ---- ROUTE 下刷队列 ---- */

/**
 * @brief 创建 ROUTE 下刷工作队列
 * @return 新建的 bgp_route_flush_queue_t 指针
 */
bgp_route_flush_queue_t *bgp_route_flush_queue_create(void);

/**
 * @brief 销毁 ROUTE 下刷工作队列（释放所有未处理条目）
 * @param q bgp_route_flush_queue_t 指针（允许为 NULL）
 * @param inst 所属实例（用于释放 rthead 引用，可为 NULL）
 */
void bgp_route_flush_queue_destroy(bgp_route_flush_queue_t *q, bgp_instance_t *inst);

/**
 * @brief 将 rthead 推入 ROUTE 下刷队列
 * @param q    下刷队列
 * @param head rthead 指针（入队时加引用）
 * @return 0 成功，-1 参数无效
 */
int bgp_route_flush_queue_push(bgp_route_flush_queue_t *q, bgp_rthead_t *head);

/**
 * @brief 批量处理 ROUTE 下刷队列（每次处理至多 batch_size 条）
 * @param q          下刷队列
 * @param inst       目标地址族实例
 * @param batch_size 本次处理上限
 * @return 实际处理条目数
 */
int bgp_route_flush_queue_process(bgp_route_flush_queue_t *q, bgp_instance_t *inst, int batch_size);

/**
 * @brief 向实例下所有 ESTABLISHED 邻居同步发送 WITHDRAW 报文
 *
 * 由 bgp_calc_run_one() 在无路由时同步调用，不经过 pub_queue。
 *
 * @param inst 目标地址族实例
 * @param nlri 待撤销的 NLRI（借用引用，仅读取内容）
 */
void bgp_work_send_withdraw_to_all(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 处理一条 BGP calc 工作事件
 * @param vrf_id 目标 VRF
 * @param afi    地址族
 * @param safi   子地址族
 */
void bgp_work_handle_calc_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 处理一条 BGP route-flush 工作事件
 * @param vrf_id 目标 VRF
 * @param afi    地址族
 * @param safi   子地址族
 */
void bgp_work_handle_route_flush_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 处理一条 BGP session-pub 工作事件
 * @param vrf_id 目标 VRF
 * @param afi    地址族
 * @param safi   子地址族
 */
void bgp_work_handle_session_pub_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 在当前线程内同步抽干实例的所有数据队列
 *
 * 用于配置删除/协议关闭路径，确保销毁前完成已排队的数据队列处理。
 *
 * @param inst 目标地址族实例
 */
void bgp_work_process_pending(bgp_instance_t *inst);

#endif /* BGP_WORK_H */
