/**
 * @file   sbmp_rib.h
 * @brief  SBMP 内存路由表：独立于 bgp_rib，按 NLRI 二进制比较索引
 * @author jhb
 * @date   2026/03/15
 */
#ifndef SBMP_RIB_H
#define SBMP_RIB_H

#include <glib.h>
#include <stdint.h>

#include "bgp.h"
#include "net_addr.h"

/**
 * @brief SBMP 单条路由（同一 NLRI 下按来源邻居 IP 区分）
 */
typedef struct sbmp_route
{
    net_addr_t source;      /**< 来源邻居 IP（二进制） */
    bgp_attr_t attr;        /**< 路径属性 */
    bgp_nexthop_t nexthop;  /**< 下一跳 */
    gint64 updated_at_usec; /**< 最近更新时间（g_get_real_time） */
} sbmp_route_t;

/**
 * @brief SBMP 路由头：唯一 NLRI 前缀，下挂多条来源路径
 *
 * 树键直接指向 nlri 字段（&head->nlri），无需额外堆分配。
 */
typedef struct sbmp_rthead
{
    bgp_nlri_entry_t nlri;  /**< NLRI（含 afi/safi/type/binary fields/key） */
    GHashTable *route_hash; /**< net_addr_t* -> sbmp_route_t*（按来源 IP 索引） */
} sbmp_rthead_t;

/**
 * @brief SBMP 内存路由表
 *
 * 独立于 bgp_rib_t，无 bgp_instance_t 反向引用，
 * bgp_parse 库只负责解析，路由存储由 SBMP 自己管理。
 */
typedef struct sbmp_rib
{
    GTree *head_tree;     /**< key = &head->nlri，按 NLRI 二进制比较 */
    uint32_t head_count;  /**< rthead 总数 */
    uint32_t route_count; /**< route 总数（所有 rthead 下累计） */
} sbmp_rib_t;

/**
 * @brief UPDATE 应用统计
 */
typedef struct sbmp_rib_update_stats
{
    uint32_t reach_new;       /**< reach 新增路径数 */
    uint32_t reach_update;    /**< reach 覆盖更新路径数 */
    uint32_t unreach_removed; /**< unreach 成功删除路径数 */
    uint32_t unreach_miss;    /**< unreach 未命中路径数 */
} sbmp_rib_update_stats_t;

/**
 * @brief 创建 SBMP RIB
 */
sbmp_rib_t *sbmp_rib_create(void);

/**
 * @brief 销毁 SBMP RIB
 */
void sbmp_rib_destroy(sbmp_rib_t *rib);

/**
 * @brief 对单条 NLRI 执行 reach（新增/更新一条来源路径）
 * @return 1=新增, 0=更新, -1=失败
 */
int sbmp_rib_reach_one(sbmp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source, const bgp_attr_t *attr,
                       const bgp_nexthop_t *nexthop);

/**
 * @brief 对单条 NLRI 执行 unreach（删除一条来源路径）
 * @return 1=删除成功, 0=未命中, -1=失败
 */
int sbmp_rib_unreach_one(sbmp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source);

/**
 * @brief 将 bgp_update_result_t 应用到 SBMP RIB
 */
void sbmp_rib_apply_update(sbmp_rib_t *rib, const net_addr_t *source, const bgp_update_result_t *upd,
                           sbmp_rib_update_stats_t *stats);

/**
 * @brief 删除某来源在整个 RIB 下的所有路径（会清理空 rthead）
 */
void sbmp_rib_remove_source(sbmp_rib_t *rib, const net_addr_t *source, uint32_t *removed_routes,
                            uint32_t *removed_heads);

/**
 * @brief 通过 NLRI 查找 rthead（只读）
 */
const sbmp_rthead_t *sbmp_rib_lookup_head(const sbmp_rib_t *rib, const bgp_nlri_entry_t *nlri);

/**
 * @brief 在 rthead 下按 source 查找 route（只读）
 */
const sbmp_route_t *sbmp_rthead_lookup_route(const sbmp_rthead_t *head, const net_addr_t *source);

/**
 * @brief 获取统计值
 */
uint32_t sbmp_rib_head_count(const sbmp_rib_t *rib);
uint32_t sbmp_rib_route_count(const sbmp_rib_t *rib);

#endif /* SBMP_RIB_H */
