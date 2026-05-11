/**
 * @file   pending.h
 * @brief  通用挂起表：缺依赖时挂起数据，依赖到达时自动重试
 * @author jhb
 * @date   2026/05/11
 *
 * @details 用于解决跨模块依赖时序问题。典型场景：IF 模块恢复配置时引用了
 *          某 VRF，但该 VRF 数据尚未到达。此时将该接口配置 park 到 (DEP_VRF, vrf_id)
 *          下；待 VRF 订阅事件到达时，调用 pending_resolve 触发重试。
 *
 *          线程模型：每张挂起表只允许在一个线程内访问（通常是该模块的 IPC
 *          handler 线程）。内部不加锁。
 */

#ifndef PENDING_H
#define PENDING_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/** 挂起表前向声明 */
typedef struct pending pending_t;

/** 重试回调返回值：处理成功，从挂起表摘除 */
#define PENDING_DONE 0

/** 重试回调返回值：仍缺其他依赖，retry_fn 须自行重新调用 pending_park 挂起到新 key */
#define PENDING_AGAIN 1

/* 负数表示永久失败：从挂起表摘除并记录错误日志 */

/**
 * @brief 挂起项重试回调
 * @param item 挂起时保存的数据指针（由 item_size 决定是副本还是原引用）
 * @param ctx  park 时传入的上下文
 * @return PENDING_DONE / PENDING_AGAIN / 负数错误码
 */
typedef int (*pending_retry_fn)(void *item, void *ctx);

/**
 * @brief 挂起项的释放回调（仅在 item_size==0 的引用模式下被调用）
 * @param item 挂起时传入的数据指针
 */
typedef void (*pending_item_free_fn)(void *item);

/**
 * @brief show 命令用的挂起项格式化回调
 */
typedef void (*pending_item_dump_fn)(FILE *out, uint32_t dep_type, uint64_t dep_key, const void *item);

/**
 * @brief 创建挂起表
 * @param name 表名（用于日志和 show 输出，内部复制）
 * @return 挂起表指针，失败返回 NULL
 */
pending_t *pending_new(const char *name);

/**
 * @brief 销毁挂起表，释放所有挂起项
 */
void pending_destroy(pending_t *p);

/**
 * @brief 将一条数据挂起，等待依赖到达
 * @param p         挂起表
 * @param dep_type  依赖类型（模块自定义，如 DEP_VRF / DEP_IF / DEP_NEXTHOP）
 * @param dep_key   依赖键值
 * @param item      数据指针
 * @param item_size 数据大小；>0 时挂起表复制一份副本（推荐），==0 时按引用挂起
 * @param free_fn   引用模式下用于释放 item 的回调，可为 NULL；副本模式忽略
 * @param fn        依赖到达时的重试回调，不允许为 NULL
 * @param ctx       透传给回调的上下文
 */
void pending_park(pending_t *p, uint32_t dep_type, uint64_t dep_key, const void *item, size_t item_size,
                  pending_item_free_fn free_fn, pending_retry_fn fn, void *ctx);

/**
 * @brief 依赖到达，触发等它的所有挂起项重试
 * @details 重试期间 retry_fn 可再次调用 pending_park 重新挂起到其他 key；
 *          本次 resolve 已经取出的列表不受影响。
 * @return 重试成功（返回 PENDING_DONE）的项数
 */
int pending_resolve(pending_t *p, uint32_t dep_type, uint64_t dep_key);

/**
 * @brief 依赖被删除：丢弃等它的所有挂起项（不调用 retry_fn）
 * @return 被丢弃的项数
 */
int pending_invalidate(pending_t *p, uint32_t dep_type, uint64_t dep_key);

/**
 * @brief 当前挂起项总数（所有 key 的总和）
 */
size_t pending_count(const pending_t *p);

/**
 * @brief 输出挂起表内容（show 命令用）
 * @param p         挂起表
 * @param out       输出流，NULL 表示 stderr
 * @param dump_item 每项的格式化函数，NULL 表示只打印类型/键/挂起时长
 */
void pending_dump(const pending_t *p, FILE *out, pending_item_dump_fn dump_item);

#endif /* PENDING_H */
