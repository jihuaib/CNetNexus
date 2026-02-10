/**
 * @file   ipc_query.h
 * @brief  IPC 同步查询支持（condvar 实现）头文件
 * @author jhb
 * @date   2026/02/02
 */

#ifndef IPC_QUERY_H
#define IPC_QUERY_H

#include <glib.h>
#include <pthread.h>
#include <stdint.h>

#include "ipc.h"

/** 挂起的同步查询 */
typedef struct ipc_pending_query
{
    uint32_t request_id;     /**< 请求 ID */
    ipc_message_t *response; /**< 响应消息（由 IO 线程设置） */
    pthread_mutex_t mutex;   /**< 互斥锁 */
    pthread_cond_t cond;     /**< 条件变量 */
    int completed;           /**< 是否已完成 */
} ipc_pending_query_t;

/** 查询管理器 */
typedef struct ipc_query_mgr
{
    GHashTable *pending;  /**< request_id -> ipc_pending_query_t* */
    pthread_mutex_t lock; /**< 全局锁 */
    uint32_t next_id;     /**< 下一个请求 ID */
} ipc_query_mgr_t;

/**
 * @brief 初始化查询管理器
 * @return 查询管理器
 */
ipc_query_mgr_t *ipc_query_mgr_create(void);

/**
 * @brief 销毁查询管理器
 * @param mgr 查询管理器
 */
void ipc_query_mgr_destroy(ipc_query_mgr_t *mgr);

/**
 * @brief 分配新请求 ID 并注册挂起查询
 * @param mgr 查询管理器
 * @return 请求 ID
 */
uint32_t ipc_query_mgr_register(ipc_query_mgr_t *mgr);

/**
 * @brief 等待查询响应
 * @param mgr 查询管理器
 * @param request_id 请求 ID
 * @param timeout_ms 超时时间（毫秒）
 * @return 响应消息，超时返回 NULL
 */
ipc_message_t *ipc_query_mgr_wait(ipc_query_mgr_t *mgr, uint32_t request_id, uint32_t timeout_ms);

/**
 * @brief 完成挂起的查询（由 IO 线程调用）
 * @param mgr 查询管理器
 * @param request_id 请求 ID
 * @param response 响应消息（所有权转移给等待者）
 * @return 成功返回 0（找到对应挂起查询），未找到返回 -1
 */
int ipc_query_mgr_complete(ipc_query_mgr_t *mgr, uint32_t request_id, ipc_message_t *response);

#endif // IPC_QUERY_H
