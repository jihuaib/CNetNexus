/**
 * @file   bgp_attr_intern.h
 * @brief  BGP 路径属性：去重（intern）存储 + 协议语义 helper
 * @author jhb
 * @date   2026/04/09
 *
 * 本头文件提供两层能力：
 *   1. 存储层：bgp_attr_intern/release/find_by_id —— 属性去重、引用计数、attr_id 管理
 *   2. 语义层：bgp_attr_as_path_contains_as / is_as_loop / build_imported / nexthop_from_addr
 *             —— 不依赖 runtime 上下文的纯属性操作
 */
#ifndef BGP_ATTR_INTERN_H
#define BGP_ATTR_INTERN_H

#include <glib.h>
#include <stdint.h>

#include "bgp.h"
#include "net_addr.h"

/** 属性来源标识位（可多 bit 共存：同一 attr 可能同时被 loc-rib 与 rib-out 持有） */
#define BGP_ATTR_SRC_LOC_RIB (1U << 0) /**< 来自本地 RIB（接收/导入路径） */
#define BGP_ATTR_SRC_RIB_OUT (1U << 1) /**< 来自出向 Adj-RIB-Out（发布路径评估后） */

/**
 * @brief 带引用计数的共享属性
 *
 * intern 后不可修改 attr 内容；需要变更属性时应 release 旧引用、intern 新值。
 */
typedef struct bgp_attr_ref
{
    bgp_attr_t attr;       /**< 路径属性（不可变） */
    uint32_t refcnt;       /**< 引用计数 */
    uint32_t hash;         /**< 预计算 hash 值（加速查找） */
    uint32_t attr_id;      /**< 全局唯一属性 ID（自增，从 1 开始） */
    uint32_t source_flags; /**< 来源位图（BGP_ATTR_SRC_*），按 OR 累积 */
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
 * 无论新建还是复用，source_flag 都会 OR 合并到 ref->source_flags。
 *
 * @param attr        待 intern 的属性（内容被复制，调用方可释放原始值）
 * @param source_flag 来源位图（BGP_ATTR_SRC_*，允许为 0）
 * @return 共享引用指针（不为 NULL）
 */
bgp_attr_ref_t *bgp_attr_intern(const bgp_attr_t *attr, uint32_t source_flag);

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

/**
 * @brief intern 表遍历回调
 * @param ref       当前属性引用（借用，不可修改）
 * @param user_data 用户参数
 */
typedef void (*bgp_attr_intern_cb)(const bgp_attr_ref_t *ref, gpointer user_data);

/**
 * @brief 遍历 intern 表所有属性（顺序不保证；回调期间禁止修改表）
 */
void bgp_attr_intern_foreach(bgp_attr_intern_cb cb, gpointer user_data);

/* ============================================================================
 * 属性语义 helper（与存储无关，可在任意上下文调用）
 * ========================================================================== */

/**
 * @brief 判断 AS-path 字符串中是否包含指定 AS
 *
 * AS-path 格式示例："65001 65002 {65010,65011} 65003"，
 * 空格/制表符/花括号/逗号均作为分隔符处理。
 *
 * @param as_path AS-path 字符串（允许 NULL 或空串）
 * @param asn     目标 AS 号（0 直接返回 FALSE）
 * @return TRUE 若 asn 出现在 as_path 中
 */
gboolean bgp_attr_as_path_contains_as(const char *as_path, uint32_t asn);

/**
 * @brief 判断属性是否构成 AS-loop（AS_PATH 中包含 local_as）
 *
 * 便捷包装，等价于：attr && local_as != 0 && as_path_contains_as(attr->as_path, local_as)
 */
gboolean bgp_attr_is_as_loop(const bgp_attr_t *attr, uint32_t local_as);

/**
 * @brief 构造"外部协议导入到 BGP"的合成属性
 *
 * 输出：origin=INCOMPLETE, local_pref=100, has_local_pref=true, 其余字段清零。
 * 用于 import-route 场景，调用方提供栈上 bgp_attr_t。
 */
void bgp_attr_build_imported(bgp_attr_t *attr);

/**
 * @brief 由一个 net_addr_t 构造仅含 global 的 nexthop（无 link-local）
 */
void bgp_nexthop_from_addr(bgp_nexthop_t *nh, const net_addr_t *addr);

#endif /* BGP_ATTR_INTERN_H */
