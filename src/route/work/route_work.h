/**
 * @file   route_work.h
 * @brief  Route 工作队列（优选队列 + timerfd 批量处理）
 * @author jhb
 * @date   2026/03/28
 */
#ifndef ROUTE_WORK_H
#define ROUTE_WORK_H

#include <glib.h>
#include <stdint.h>

#include "route_rib.h"

/** 每次定时器触发时处理的最大条目数 */
#define ROUTE_WORK_BATCH_SIZE 64

/** 工作定时器触发间隔（毫秒） */
#define ROUTE_WORK_TIMER_INTERVAL_MS 100

/** 工作定时器 epoll 哨兵类型标识 */
#define ROUTE_WORK_TIMER_TYPE 1

// ============================================================================
// epoll 哨兵
// ============================================================================

/**
 * @brief 工作定时器 epoll 哨兵
 *
 * 注册到 epoll 时 data.ptr = ((uintptr_t)sentinel | 1)（bit0=1 标记为定时器事件）。
 */
typedef struct route_work_sentinel
{
    int type; /**< 固定为 ROUTE_WORK_TIMER_TYPE */
} route_work_sentinel_t;

// ============================================================================
// 优选队列
// ============================================================================

/**
 * @brief 优选工作队列
 *
 * FIFO 队列，路由 ADD/UPDATE 变化时入队，定时器批量出队处理。
 * 元素为 route_head_key_t* 的深拷贝（由队列持有所有权）。
 * DELETE 不走此队列：在 route_rib_del 回调中同步触发优选计算。
 */
typedef struct route_calc_queue
{
    GQueue *q;      /**< FIFO 队列（元素为 route_head_key_t*） */
    uint32_t count; /**< 当前队列中的条目数 */
} route_calc_queue_t;

// ============================================================================
// API
// ============================================================================

/**
 * @brief 设置全局 epoll fd（由 route_worker 在 epoll 创建后调用）
 * @param epoll_fd 已创建的 epoll 文件描述符
 */
void route_work_set_epoll_fd(int epoll_fd);

/**
 * @brief 创建优选工作队列
 * @return 新建的 route_calc_queue_t 指针，失败返回 NULL
 */
route_calc_queue_t *route_calc_queue_create(void);

/**
 * @brief 销毁优选工作队列（释放所有未处理条目）
 * @param q route_calc_queue_t 指针（允许为 NULL）
 */
void route_calc_queue_destroy(route_calc_queue_t *q);

/**
 * @brief 将前缀头键入队（深拷贝 key）
 * @param q   优选队列
 * @param key 前缀头键（借用引用，函数内部复制）
 * @return 0 成功，-1 参数无效
 */
int route_calc_queue_push(route_calc_queue_t *q, const route_head_key_t *key);

/**
 * @brief 批量处理优选队列（每次处理至多 batch_size 条）
 *
 * 从队列出队 head_key，在 RIB 中查找对应 head，若存在则调用
 * route_calc_on_path_add() 触发优选计算、OS 同步和订阅者通知。
 *
 * @param q          优选队列
 * @param batch_size 本次处理上限
 * @return 实际处理条目数
 */
int route_calc_queue_process(route_calc_queue_t *q, int batch_size);

/**
 * @brief 启动工作定时器并注册到 epoll
 * @param sentinel    哨兵结构体指针（由调用者持有生命周期）
 * @param timerfd_out 输出：创建的 timerfd（调用者负责管理）
 * @param interval_ms 定时器触发间隔（毫秒）
 * @return 0 成功，-1 失败
 */
int route_work_timer_start(route_work_sentinel_t *sentinel, int *timerfd_out, uint32_t interval_ms);

/**
 * @brief 停止并关闭工作定时器，从 epoll 注销
 * @param timerfd timerfd 指针（停止后置为 -1）
 */
void route_work_timer_stop(int *timerfd);

/**
 * @brief 工作定时器触发处理入口（由 route_worker epoll 循环调用）
 *
 * 读取 timerfd 计数，批量处理 calc_queue。
 *
 * @param timerfd 工作定时器 fd
 * @param q       优选队列
 */
void route_work_process(int timerfd, route_calc_queue_t *q);

#endif /* ROUTE_WORK_H */
