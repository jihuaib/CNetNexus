/**
 * @file   bgp_work.h
 * @brief  BGP 路由工作处理
 * @author jhb
 * @date   2026/03/15
 */
#ifndef BGP_WORK_H
#define BGP_WORK_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

/* 包含顺序：bgp_peer.h 定义 bgp_afi_t/bgp_safi_t 枚举，必须先于 bgp.h（定义同名宏） */
#include "bgp.h"
#include "bgp_peer.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_update_group.h"

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

/* ---- Subgroup 级发布通路（新数据通路） ---- */

/**
 * @brief 子组级出向策略评估
 *
 * 整合属性准备与 nexthop 策略：
 *   1. 复制 best 属性到 out_attr
 *   2. eBGP 场景下在 AS_PATH 首部 prepend 本地 AS
 *   3. 按子组 nh_policy / effective_local_addr 生成 out_nh
 *      - IMPORT 路由：使用 effective_local_addr（同族/双栈/RFC 8950）
 *      - 非 IMPORT 路由：保留 best->nexthop
 *   4. iBGP→iBGP 反射检查（整个子组 sess_type 一致）
 *
 * Per-session 检查（split-horizon、AS_PATH 防环）延后到发送阶段执行。
 *
 * @param sg       子组
 * @param best     best 路径（非 NULL）
 * @param nlri     关联 NLRI（仅用于日志）
 * @param out_attr 输出属性
 * @param out_nh   输出 nexthop
 * @return true=应宣告, false=组级策略拒绝
 */
bool bgp_subgroup_eval_export(const bgp_nh_subgroup_t *sg, const bgp_route_node_t *best, const bgp_nlri_entry_t *nlri,
                              bgp_attr_t *out_attr, bgp_nexthop_t *out_nh);

/**
 * @brief 将一条 best-route 变更挂入该实例所有 subgroup 的 announce_queue
 *
 * 对每个 subgroup 执行 eval_export → intern → adj_rib_out_update，
 * 仅在属性或 nexthop 相对上次宣告发生变化时入队。
 *
 * @param inst 目标地址族实例
 * @param nlri 待宣告 NLRI
 */
void bgp_work_enqueue_announce_to_subgroups(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 将一条 NLRI 撤销挂入该实例所有 subgroup 的 withdraw_queue
 *
 * 仅对 adj_rib_out 中已存在条目的 subgroup 入队（避免冗余撤销）。
 *
 * @param inst 目标地址族实例
 * @param nlri 待撤销 NLRI
 */
void bgp_work_enqueue_withdraw_to_subgroups(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 邻居进入 ESTABLISHED 后，将其加入各 AF subgroup 并补发路由
 *
 * 对 sess->peer_list 中每个 peer：
 *   1. bgp_subgroup_peer_join(peer, sess)
 *   2. 若子组 adj_rib_out 为空（通常是新子组），全量遍历 RIB best 触发入队
 *   3. 若子组 adj_rib_out 已非空（已有其他 session），从 adj_rib_out 重新入队
 *      announce_queue（对新 session 补发；对老 session 会重复发送但 UPDATE 幂等）
 *
 * @param sess 目标 session
 */
void bgp_work_subgroup_catchup_session(bgp_session_t *sess);

/**
 * @brief 批量处理 subgroup 的 announce/withdraw 队列（Phase 2 单条发送）
 *
 * 先处理 withdraw_queue，再处理 announce_queue：
 *   - withdraw：对每条 NLRI，向子组内每个 ESTABLISHED session 发 WITHDRAW
 *   - announce：从 adj_rib_out 读取 (attr, nh)，对每个 session 逐一做
 *               split-horizon / AS_PATH 防环检查后发送 UPDATE
 *
 * Phase 3 会替换为打包发送。
 *
 * @param sg         子组
 * @param inst       所属 AF 实例
 * @param batch_size 本次处理上限
 * @return 实际处理的 NLRI 总数
 */
int bgp_subgroup_process_queues(bgp_nh_subgroup_t *sg, bgp_instance_t *inst, int batch_size);

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
