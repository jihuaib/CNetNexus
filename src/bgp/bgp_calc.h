/**
 * @file   bgp_calc.h
 * @brief  BGP 路由优选接口（Loc-RIB 最优路径计算与优选链表）
 * @author jhb
 * @date   2026/03/15
 */
#ifndef BGP_CALC_H
#define BGP_CALC_H

#include <glib.h>
#include <stdint.h>

/* 包含顺序：bgp_instance.h 传递包含 bgp_peer.h（定义枚举），必须先于 bgp.h（定义同名宏） */
#include "bgp.h"
#include "bgp_instance.h"

/**
 * @brief 单条优选路由条目（Loc-RIB 中每个 NLRI 的最优路径）
 *
 * 所有字段均为值语义（固定数组，无堆指针），可直接 memcpy / g_free。
 * 若将来添加堆指针字段，统一在 bgp_bestpath_entry_free() 中处理释放。
 */
typedef struct bgp_bestpath_entry
{
    bgp_nlri_entry_t nlri; /**< NLRI（含 afi/safi/type/prefix） */
    bgp_attr_t attr;       /**< 最优路径的路径属性 */
    bgp_nexthop_t nexthop; /**< 最优路径的下一跳 */
    net_addr_t source;     /**< 最优路径来源（获胜对端的 IP 地址） */
} bgp_bestpath_entry_t;

/**
 * @brief 路由优选链表（Loc-RIB）
 *
 * 挂在 bgp_instance_t 上，记录该 AF 下每个 NLRI 的当前最优路径。
 * 由 bgp_calc_run() 维护：路由变化时调用 bgp_bestlist_set() 更新或
 * bgp_bestlist_del() 删除，保证与 RIB 同步。
 */
typedef struct bgp_bestlist
{
    GList *entries; /**< bgp_bestpath_entry_t* 链表（持有所有权） */
    uint32_t count; /**< 当前条目数 */
} bgp_bestlist_t;

// ============================================================================
// 链表生命周期
// ============================================================================

/**
 * @brief 创建优选链表
 * @return 新建的 bgp_bestlist_t 指针
 */
bgp_bestlist_t *bgp_bestlist_create(void);

/**
 * @brief 销毁优选链表（释放所有条目）
 * @param list bgp_bestlist_t 指针（允许为 NULL）
 */
void bgp_bestlist_destroy(bgp_bestlist_t *list);

// ============================================================================
// 条目管理 API（供 bgp_calc_run 使用）
// ============================================================================

/**
 * @brief 新增或更新一条优选路由条目
 *
 * 按 NLRI 二进制内容查找：存在则更新 attr/nexthop/source；不存在则新增。
 * 深拷贝所有字段，调用方无需保持传入指针的生命周期。
 *
 * @param list    优选链表
 * @param nlri    NLRI（深拷贝）
 * @param attr    路径属性（深拷贝）
 * @param nexthop 下一跳（深拷贝）
 * @param source  获胜对端地址（深拷贝）
 * @return 0 成功，-1 参数无效
 */
int bgp_bestlist_set(bgp_bestlist_t *list, const bgp_nlri_entry_t *nlri, const bgp_attr_t *attr,
                     const bgp_nexthop_t *nexthop, const net_addr_t *source);

/**
 * @brief 按 NLRI 删除一条优选路由条目
 *
 * @param list 优选链表
 * @param nlri NLRI 匹配键
 * @return 0 成功（已删除），-1 未找到或参数无效
 */
int bgp_bestlist_del(bgp_bestlist_t *list, const bgp_nlri_entry_t *nlri);

/**
 * @brief 按 NLRI 查找优选路由条目（只读）
 *
 * @param list 优选链表
 * @param nlri NLRI 匹配键
 * @return 条目指针（借用，调用方不可释放），未找到返回 NULL
 */
const bgp_bestpath_entry_t *bgp_bestlist_find(const bgp_bestlist_t *list, const bgp_nlri_entry_t *nlri);

// ============================================================================
// 路由优选入口
// ============================================================================

/**
 * @brief 触发指定实例的全量路由优选计算（占位）
 *
 * 遍历 inst->rib，对每个 NLRI 执行最优路径选择，通过 bgp_bestlist_set() /
 * bgp_bestlist_del() 更新 inst->bestlist。当前为占位实现。
 *
 * @param inst 目标地址族实例
 * @return 0 成功，-1 失败
 */
int bgp_calc_run(bgp_instance_t *inst);

/**
 * @brief 对单条 NLRI 执行 best-path 计算（由工作队列定时调用）
 *
 * 通过 NLRI 在 inst->rib 中定位 rthead，选出最优路径：
 *   - rthead 不存在或路径哈希为空：同步发送 WITHDRAW（调用
 *     bgp_work_send_withdraw_to_all），再从 bestlist 删除
 *   - 有路由：选出最优路径，更新 bestlist，推 ANNOUNCE 到 pub_queue
 *
 * @param inst 目标地址族实例
 * @param nlri NLRI 条目（由 calc_queue 条目提供）
 */
void bgp_calc_run_one(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

#endif /* BGP_CALC_H */
