/**
 * @file   bgp_work.h
 * @brief  BGP 路由处理工作队列（优选队列 + 发布队列，定时批量处理）
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

/** 每次定时器触发时每个队列处理的最大条目数 */
#define BGP_WORK_BATCH_SIZE 64

/** 工作定时器触发间隔（毫秒） */
#define BGP_WORK_TIMER_INTERVAL_MS 100

// ============================================================================
// 优选队列（calc_queue）
// ============================================================================

/**
 * @brief 优选工作队列
 *
 * FIFO 队列，路由变化时入队，定时器批量出队处理。
 * GQueue 元素为 bgp_nlri_entry_t*（NLRI 值拷贝，队列持有所有权，出队后释放）。
 */
typedef struct bgp_calc_queue
{
    GQueue *q;      /**< FIFO 队列（元素为 bgp_nlri_entry_t*，值拷贝，队列持有所有权） */
    uint32_t count; /**< 当前队列中的条目数 */
} bgp_calc_queue_t;

// ============================================================================
// 发布队列（pub_queue）
// ============================================================================

/**
 * @brief 发布工作队列
 *
 * FIFO 队列，best-path 完成后入队，定时器批量出队向邻居发包。
 * 只处理 ANNOUNCE：处理时通过 NLRI 在 bestlist 中查找最优路径信息。
 * WITHDRAW 由 bgp_calc_run_one() 同步调用 bgp_work_send_withdraw_to_all() 发出。
 * GQueue 元素为 bgp_nlri_entry_t*（NLRI 值拷贝，队列持有所有权，出队后释放）。
 */
typedef struct bgp_pub_queue
{
    GQueue *q;      /**< FIFO 队列（元素为 bgp_nlri_entry_t*，值拷贝，队列持有所有权） */
    uint32_t count; /**< 当前队列中的条目数 */
} bgp_pub_queue_t;

// ============================================================================
// epoll 哨兵（与 bgp_timer_sentinel_t 内存布局兼容）
// ============================================================================

/**
 * @brief 工作定时器 epoll 哨兵
 *
 * 内存布局与 bgp_timer_sentinel_t 对齐（offset 0：_dummy，offset 8：type），
 * 因此可安全将指针转型为 bgp_timer_sentinel_t* 后读取 type 字段，
 * 确认为 BGP_TIMER_TYPE_WORK 后再转型回本结构体访问 inst。
 */
typedef struct bgp_work_sentinel
{
    struct bgp_session *_dummy; /**< 占位（对应 bgp_timer_sentinel_t.session），始终为 NULL */
    bgp_timer_type_t type;      /**< 固定为 BGP_TIMER_TYPE_WORK */
    bgp_instance_t *inst;       /**< 所属地址族实例（借用引用，不持有所有权） */
} bgp_work_sentinel_t;

// ============================================================================
// API
// ============================================================================

/**
 * @brief 设置全局 epoll fd（由 bgp_main 在 epoll 创建后调用）
 * @param epoll_fd 已创建的 epoll 文件描述符
 */
void bgp_work_set_epoll_fd(int epoll_fd);

/* ---- 优选队列 ---- */

/**
 * @brief 创建优选工作队列
 * @return 新建的 bgp_calc_queue_t 指针
 */
bgp_calc_queue_t *bgp_calc_queue_create(void);

/**
 * @brief 销毁优选工作队列（释放所有未处理条目）
 * @param q bgp_calc_queue_t 指针（允许为 NULL）
 */
void bgp_calc_queue_destroy(bgp_calc_queue_t *q);

/**
 * @brief 将 NLRI 推入优选队列
 * @param q    优选队列
 * @param nlri NLRI 条目（值拷贝）
 * @return 0 成功，-1 参数无效
 */
int bgp_calc_queue_push(bgp_calc_queue_t *q, const bgp_nlri_entry_t *nlri);

/**
 * @brief 批量处理优选队列（每次处理至多 batch_size 条）
 *
 * 从 calc_queue 出队 NLRI，调用 bgp_calc_run_one()，
 * 结果（宣告或撤销）由 bgp_calc_run_one 内部推入 inst->pub_queue。
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
 * @brief 将 NLRI 推入发布队列（仅 ANNOUNCE）
 *
 * pub_queue 存储 NLRI 值拷贝；处理时通过 NLRI 在 bestlist 中查找完整路径信息。
 *
 * @param q    发布队列
 * @param nlri NLRI 条目（值拷贝）
 * @return 0 成功，-1 参数无效
 */
int bgp_pub_queue_push(bgp_pub_queue_t *q, const bgp_nlri_entry_t *nlri);

/**
 * @brief 批量处理发布队列（每次处理至多 batch_size 条）
 *
 * 从 pub_queue 出队任务，向该实例所有 ESTABLISHED 邻居
 * 发送 UPDATE（ANNOUNCE）或 WITHDRAW 报文，处理完立即释放条目。
 *
 * @param q          发布队列
 * @param inst       目标地址族实例
 * @param batch_size 本次处理上限
 * @return 实际处理条目数
 */
int bgp_pub_queue_process(bgp_pub_queue_t *q, bgp_instance_t *inst, int batch_size);

/**
 * @brief 向实例下所有 ESTABLISHED 邻居同步发送 WITHDRAW 报文
 *
 * 由 bgp_calc_run_one() 在无路由时同步调用，不经过 pub_queue。
 *
 * @param inst 目标地址族实例
 * @param nlri 待撤销的 NLRI（借用引用，仅读取内容）
 */
void bgp_work_send_withdraw_to_all(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/* ---- 工作定时器 ---- */

/**
 * @brief 启动工作定时器并注册到 epoll
 * @param inst        目标地址族实例
 * @param interval_ms 定时器触发间隔（毫秒）
 * @return 0 成功，-1 失败
 */
int bgp_work_timer_start(bgp_instance_t *inst, uint32_t interval_ms);

/**
 * @brief 停止并关闭工作定时器，从 epoll 注销
 * @param inst 目标地址族实例
 */
void bgp_work_timer_stop(bgp_instance_t *inst);

/**
 * @brief 工作定时器触发处理入口（由 bgp_main epoll 循环调用）
 *
 * 读取 timerfd 计数，批量处理 calc_queue 和 pub_queue。
 *
 * @param inst 目标地址族实例
 */
void bgp_work_process(bgp_instance_t *inst);

#endif /* BGP_WORK_H */
