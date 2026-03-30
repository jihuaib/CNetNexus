/**
 * @file   route_work.h
 * @brief  Route 工作事件处理
 * @author jhb
 * @date   2026/03/28
 */
#ifndef ROUTE_WORK_H
#define ROUTE_WORK_H

#include <stdint.h>

#include "route_rib.h"

/** 每次 work_eventfd 醒来时处理的最大事件数 */
#define ROUTE_WORK_BATCH_SIZE 64

// ============================================================================
// API
// ============================================================================

/**
 * @brief 处理一次 route calc 工作事件
 *
 * 根据事件携带的前缀键在 RIB 中查找 head，若存在则触发优选计算和后续迭代重算。
 * head 不存在表示该前缀已在事件到达前被完全删除，此时直接忽略。
 *
 * @param key 前缀头键
 */
void route_work_handle_calc_event(const route_head_key_t *key);

#endif /* ROUTE_WORK_H */
