/**
 * @file   bgp_attr_intern.h
 * @brief  BGP 路径属性去重（intern）：共享相同属性，引用计数管理，自增 attr_id
 * @author jhb
 * @date   2026/04/09
 */
#ifndef BGP_ATTR_INTERN_H
#define BGP_ATTR_INTERN_H

#include <stdint.h>

#include "bgp.h"

/**
 * @brief 带引用计数的共享属性
 *
 * intern 后不可修改 attr 内容；需要变更属性时应 release 旧引用、intern 新值。
 */
typedef struct bgp_attr_ref
{
    bgp_attr_t attr;  /**< 路径属性（不可变） */
    uint32_t refcnt;  /**< 引用计数 */
    uint32_t hash;    /**< 预计算 hash 值（加速查找） */
    uint32_t attr_id; /**< 全局唯一属性 ID（自增，从 1 开始） */
} bgp_attr_ref_t;

/**
 * @brief 从 route 取属性的便捷宏
 *
 * 用法：BGP_ROUTE_ATTR(route)->as_path
 */
#define BGP_ROUTE_ATTR(route) (&(route)->attr->attr)

/** 初始化全局 intern 表（在 bgp_module_init 中调用） */
void bgp_attr_intern_init(void);

/** 销毁全局 intern 表 */
void bgp_attr_intern_fini(void);

/**
 * @brief 查找或插入属性，返回共享引用（refcnt 已 +1）
 *
 * 若表中已存在内容相同的属性，返回已有引用并 refcnt +1。
 * 若不存在，分配新 bgp_attr_ref_t 并插入表中，refcnt=1，分配新 attr_id。
 *
 * @param attr 待 intern 的属性（内容被复制，调用方可释放原始值）
 * @return 共享引用指针（不为 NULL）
 */
bgp_attr_ref_t *bgp_attr_intern(const bgp_attr_t *attr);

/**
 * @brief 增加引用计数
 * @param ref 共享引用（允许为 NULL）
 */
void bgp_attr_ref_get(bgp_attr_ref_t *ref);

/**
 * @brief 减少引用计数，归零时从 intern 表中删除并释放
 * @param ref 共享引用（允许为 NULL）
 */
void bgp_attr_release(bgp_attr_ref_t *ref);

/**
 * @brief 通过 attr_id 查找属性（只读）
 * @param attr_id 属性 ID
 * @return 属性引用指针（借用，不增加引用计数），未找到返回 NULL
 */
const bgp_attr_ref_t *bgp_attr_find_by_id(uint32_t attr_id);

/** 当前 intern 表中的唯一属性数量 */
uint32_t bgp_attr_intern_count(void);

#endif /* BGP_ATTR_INTERN_H */
