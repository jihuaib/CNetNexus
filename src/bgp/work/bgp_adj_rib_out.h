/**
 * @file   bgp_adj_rib_out.h
 * @brief  BGP Adj-RIB-Out：子组级别记录已宣告的 (attr_ref, nexthop)，支持增量发布
 * @author jhb
 * @date   2026/04/10
 *
 * 约束：
 *   - 持有 bgp_attr_ref_t 的引用计数（插入时 get，移除/清空/覆盖时 release）
 *   - 以 bgp_nlri_entry_t 堆副本为键，bgp_adj_rib_out_entry_t 堆副本为值
 *   - 同一子组内所有 session 在 bgp_subgroup_process_queues 出队后共享一致状态
 */
#ifndef BGP_ADJ_RIB_OUT_H
#define BGP_ADJ_RIB_OUT_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "bgp.h"
#include "bgp_attr_intern.h"

/* 前向声明，供外部以指针形式引用（如 bgp_update_group.h） */
typedef struct bgp_adj_rib_out bgp_adj_rib_out_t;

/** Adj-RIB-Out 条目（一条已宣告的 NLRI 快照） */
typedef struct bgp_adj_rib_out_entry
{
    bgp_attr_ref_t *attr_ref; /**< 已宣告属性（持有引用） */
    bgp_nexthop_t nexthop;    /**< 已宣告 nexthop */
    bool advertised;          /**< 是否已成功发送（Phase 3 使用；Phase 2 常为 true） */
} bgp_adj_rib_out_entry_t;

/** Adj-RIB-Out */
struct bgp_adj_rib_out
{
    GHashTable *table; /**< bgp_nlri_entry_t*(堆副本) → bgp_adj_rib_out_entry_t*(堆副本) */
    uint32_t count;    /**< 条目数 */
};

/** 变化类型 */
typedef enum bgp_aro_change
{
    BGP_ARO_UNCHANGED = 0, /**< 属性+nexthop 完全一致 */
    BGP_ARO_UPDATED = 1,   /**< 已有条目被更新（属性或 nexthop 变化） */
    BGP_ARO_NEW = 2,       /**< 新增条目 */
} bgp_aro_change_t;

/** 创建空的 Adj-RIB-Out */
bgp_adj_rib_out_t *bgp_adj_rib_out_create(void);

/** 销毁（释放所有条目及其持有的 attr_ref） */
void bgp_adj_rib_out_destroy(bgp_adj_rib_out_t *aro);

/** 清空（释放所有条目但保留 aro 结构） */
void bgp_adj_rib_out_clear(bgp_adj_rib_out_t *aro);

/** 只读查找 */
const bgp_adj_rib_out_entry_t *bgp_adj_rib_out_lookup(const bgp_adj_rib_out_t *aro, const bgp_nlri_entry_t *nlri);

/**
 * @brief 插入或更新条目
 *
 * 内部会对 attr_ref 调用 bgp_attr_ref_get（增加引用），
 * 覆盖旧条目时对旧 attr_ref 调用 bgp_attr_release。
 *
 * @param aro      目标 Adj-RIB-Out
 * @param nlri     NLRI 键（内部会创建堆副本）
 * @param attr_ref 属性共享引用（非 NULL）
 * @param nh       nexthop
 * @return 变化类型
 */
bgp_aro_change_t bgp_adj_rib_out_update(bgp_adj_rib_out_t *aro, const bgp_nlri_entry_t *nlri, bgp_attr_ref_t *attr_ref,
                                        const bgp_nexthop_t *nh);

/**
 * @brief 移除条目（对其 attr_ref 调用 bgp_attr_release）
 * @return true=已移除, false=未命中
 */
bool bgp_adj_rib_out_remove(bgp_adj_rib_out_t *aro, const bgp_nlri_entry_t *nlri);

/** 标记条目已成功发送 */
void bgp_adj_rib_out_mark_advertised(bgp_adj_rib_out_t *aro, const bgp_nlri_entry_t *nlri);

/** 条目数 */
uint32_t bgp_adj_rib_out_count(const bgp_adj_rib_out_t *aro);

/** 遍历回调：对每条条目调用 */
typedef void (*bgp_adj_rib_out_cb)(const bgp_nlri_entry_t *nlri, const bgp_adj_rib_out_entry_t *entry,
                                   gpointer user_data);

/** 遍历所有条目 */
void bgp_adj_rib_out_foreach(const bgp_adj_rib_out_t *aro, bgp_adj_rib_out_cb cb, gpointer user_data);

#endif /* BGP_ADJ_RIB_OUT_H */
