/**
 * @file   bgp_rib.h
 * @brief  BGP 内存 RIB 通用结构：rthead（前缀头）+ route（路径）
 * @author jhb
 * @date   2026/03/13
 */
#ifndef BGP_RIB_H
#define BGP_RIB_H

#include <glib.h>
#include <stdint.h>

#include "bgp_route.h"

/** route 来源标识最大长度（通常为邻居 IP 字符串） */
#define BGP_RIB_SOURCE_MAX 64

/** rthead 树键最大长度（afi/safi + nlri key） */
#define BGP_RIB_HEAD_KEY_MAX (BGP_NLRI_KEY_MAX + 32)

/**
 * @brief 单条路径（同一 rthead 下可挂多条，按 source 区分）
 */
typedef struct bgp_route_node
{
    char source[BGP_RIB_SOURCE_MAX]; /**< 路径来源，如 "192.0.2.1" */
    bgp_attr_t attr;                 /**< 路径属性 */
    bgp_nexthop_t nexthop;           /**< 下一跳 */
    gint64 updated_at_usec;          /**< 最近更新时间（g_get_real_time） */
} bgp_route_node_t;

/**
 * @brief 路由头（Route Head）：表示一个唯一 NLRI 前缀
 */
typedef struct bgp_rthead
{
    char key[BGP_RIB_HEAD_KEY_MAX]; /**< 树键：afi/safi/nlri_key */
    bgp_nlri_entry_t nlri;          /**< NLRI（前缀/EVPN/FlowSpec 等） */
    GHashTable *route_hash;         /**< source(gchar*) -> bgp_route_node_t* */
} bgp_rthead_t;

/**
 * @brief BGP 内存 RIB
 */
typedef struct bgp_rib
{
    GTree *head_tree;     /**< key(gchar*) -> bgp_rthead_t* */
    uint32_t head_count;  /**< rthead 总数 */
    uint32_t route_count; /**< route 总数（所有 rthead 下累计） */
} bgp_rib_t;

/**
 * @brief UPDATE 应用统计
 */
typedef struct bgp_rib_update_stats
{
    uint32_t reach_new;       /**< reach 新增路径数 */
    uint32_t reach_update;    /**< reach 覆盖更新路径数 */
    uint32_t unreach_removed; /**< unreach 成功删除路径数 */
    uint32_t unreach_miss;    /**< unreach 未命中路径数 */
} bgp_rib_update_stats_t;

/**
 * @brief 创建 RIB
 */
bgp_rib_t *bgp_rib_create(void);

/**
 * @brief 销毁 RIB
 */
void bgp_rib_destroy(bgp_rib_t *rib);

/**
 * @brief 对单条 NLRI 执行 reach（新增/更新一条来源路径）
 * @param rib      目标 RIB
 * @param nlri     NLRI 条目
 * @param source   路径来源（邻居标识）
 * @param attr     路径属性
 * @param nexthop  下一跳
 * @return 1=新增路径, 0=更新已有路径, -1=失败
 */
int bgp_rib_reach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const char *source, const bgp_attr_t *attr,
                      const bgp_nexthop_t *nexthop);

/**
 * @brief 对单条 NLRI 执行 unreach（删除一条来源路径）
 * @param rib      目标 RIB
 * @param nlri     NLRI 条目
 * @param source   路径来源（邻居标识）
 * @return 1=删除成功, 0=未命中, -1=失败
 */
int bgp_rib_unreach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const char *source);

/**
 * @brief 将解析后的 UPDATE 应用于 RIB
 */
void bgp_rib_apply_update(bgp_rib_t *rib, const char *source, const bgp_update_result_t *upd,
                          bgp_rib_update_stats_t *stats);

/**
 * @brief 删除某来源在整个 RIB 下的所有路径（会清理空 rthead）
 * @param rib            目标 RIB
 * @param source         路径来源（邻居标识）
 * @param removed_routes 输出：删除路径数（可为 NULL）
 * @param removed_heads  输出：删除 rthead 数（可为 NULL）
 */
void bgp_rib_remove_source(bgp_rib_t *rib, const char *source, uint32_t *removed_routes, uint32_t *removed_heads);

/**
 * @brief 通过 NLRI 查找 rthead（只读）
 */
const bgp_rthead_t *bgp_rib_lookup_head(const bgp_rib_t *rib, const bgp_nlri_entry_t *nlri);

/**
 * @brief 在 rthead 下按 source 查找 route（只读）
 */
const bgp_route_node_t *bgp_rthead_lookup_route(const bgp_rthead_t *head, const char *source);

/**
 * @brief 获取统计值
 */
uint32_t bgp_rib_head_count(const bgp_rib_t *rib);
uint32_t bgp_rib_route_count(const bgp_rib_t *rib);

#endif /* BGP_RIB_H */
