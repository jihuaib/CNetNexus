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

#include "bgp.h"
#include "bit.h"
#include "net_addr.h"

/* 前向声明：bgp_instance_t 与 bgp_rib_t 相互引用 */
typedef struct bgp_instance bgp_instance_t;

/** 路由标记位：当前最优路径（由 bgp_calc 置位，需同时满足位于链表首位） */
#define BGP_ROUTE_FLAG_BEST (1U << 0)
/** 路由标记位：本地导入路由（非 BGP 邻居学习，由 import-route 引入） */
#define BGP_ROUTE_FLAG_IMPORT (1U << 1)
/** 路由标记位：nexthop 迭代有效（valid） */
#define BGP_ROUTE_FLAG_VALID (1U << 2)

/**
 * @brief 单条路径（同一 rthead 下可挂多条，按 source 来源地址区分）
 *
 * peer 路由：BGP_ROUTE_FLAG_IMPORT 未置位，source 为邻居 IP。
 * import 路由：BGP_ROUTE_FLAG_IMPORT 置位，source 为来源标识地址。
 * 有效路径：BGP_ROUTE_FLAG_VALID 置位。
 * 最优路径：BGP_ROUTE_FLAG_BEST + BGP_ROUTE_FLAG_VALID 均置位，且为链表首元素。
 */
typedef struct bgp_route_node
{
    net_addr_t source;      /**< 路由来源标识（peer 路由=邻居IP，import 路由=来源地址） */
    bgp_attr_t attr;        /**< 路径属性 */
    bgp_nexthop_t nexthop;  /**< 下一跳 */
    gint64 added_at_usec;   /**< 路由首次加入时间（g_get_real_time，仅新增时写入） */
    gint64 updated_at_usec; /**< 路由最近更新时间（g_get_real_time，每次 reach 写入） */
    uint32_t flags;         /**< 路由标记位，见 BGP_ROUTE_FLAG_* */
} bgp_route_node_t;

/**
 * @brief 路由头（Route Head）：表示一个唯一 NLRI 前缀
 *
 * 树键为 NLRI 二进制内容（RIB 已按 AFI/SAFI 分实例）
 */
typedef struct bgp_rthead
{
    bgp_nlri_entry_t nlri; /**< NLRI（前缀/EVPN/FlowSpec 等，含 afi/safi/type） */
    bgp_instance_t *inst;  /**< 所属 AF 实例（借用引用，可为 NULL） */
    GList *route_list;     /**< bgp_route_node_t* 双向链表，首元素为当前最优路径 */
} bgp_rthead_t;

/**
 * @brief BGP 内存 RIB
 */
typedef struct bgp_rib
{
    GTree *head_tree;     /**< key = &head->nlri（直接指入值，无需堆分配），按 NLRI 二进制比较 */
    uint32_t head_count;  /**< rthead 总数 */
    uint32_t route_count; /**< route 总数（所有 rthead 下累计） */
    bgp_instance_t *inst; /**< 所属 AF 实例（借用引用，NULL 表示独立/测试使用） */
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
 * @param rib          目标 RIB
 * @param nlri         NLRI 条目
 * @param source       路径来源（peer 路由=邻居IP，import 路由=来源地址）
 * @param import_proto 是否为 import 路由（0=peer 路由，非 0=import 路由，置 BGP_ROUTE_FLAG_IMPORT）
 * @param attr         路径属性
 * @param nexthop      下一跳
 * @return 1=新增路径, 0=更新已有路径, -1=失败
 */
int bgp_rib_reach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source, uint32_t import_proto,
                      const bgp_attr_t *attr, const bgp_nexthop_t *nexthop);

/**
 * @brief 对单条 NLRI 执行 unreach（删除一条来源路径）
 * @param rib      目标 RIB
 * @param nlri     NLRI 条目
 * @param source   路径来源（邻居 IP，二进制）
 * @return 1=删除成功, 0=未命中, -1=失败
 */
int bgp_rib_unreach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source);

/**
 * @brief 设置一条路径的 valid 状态
 * @param rib    目标 RIB
 * @param nlri   NLRI 条目
 * @param source 路径来源
 * @param valid  TRUE=有效，FALSE=无效
 * @return 1=状态有变化, 0=状态未变化/未命中, -1=失败
 */
int bgp_rib_set_route_valid(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source, gboolean valid);

/**
 * @brief 将解析后的 UPDATE 应用于 RIB
 */
void bgp_rib_apply_update(bgp_rib_t *rib, const net_addr_t *source, const bgp_update_result_t *upd,
                          bgp_rib_update_stats_t *stats);

/**
 * @brief 删除某来源在整个 RIB 下的所有路径（会清理空 rthead）
 * @param rib            目标 RIB
 * @param source         路径来源（邻居 IP，二进制）
 * @param removed_routes 输出：删除路径数（可为 NULL）
 * @param removed_heads  输出：删除 rthead 数（可为 NULL）
 */
void bgp_rib_remove_source(bgp_rib_t *rib, const net_addr_t *source, uint32_t *removed_routes, uint32_t *removed_heads);

/**
 * @brief 通过 NLRI 查找 rthead（只读）
 */
const bgp_rthead_t *bgp_rib_lookup_head(const bgp_rib_t *rib, const bgp_nlri_entry_t *nlri);

/**
 * @brief 遍历 RIB 中含有指定来源路径的所有 rthead，对每个 NLRI 触发回调
 *
 * 用于会话清理前收集受影响 NLRI，推送到 calc_queue 触发重新优选。
 *
 * @param rib       目标 RIB
 * @param source    路径来源（邻居 IP）
 * @param cb        回调函数，参数为 NLRI 指针（借用）和 user_data
 * @param user_data 传递给回调的上下文指针
 */
typedef void (*bgp_rib_source_nlri_cb)(const bgp_nlri_entry_t *nlri, gpointer user_data);
void bgp_rib_foreach_source(const bgp_rib_t *rib, const net_addr_t *source, bgp_rib_source_nlri_cb cb,
                            gpointer user_data);

/**
 * @brief 在 rthead 下按 source 查找 route（只读）
 */
const bgp_route_node_t *bgp_rthead_lookup_route(const bgp_rthead_t *head, const net_addr_t *source);

/**
 * @brief 获取统计值
 */
uint32_t bgp_rib_head_count(const bgp_rib_t *rib);
uint32_t bgp_rib_route_count(const bgp_rib_t *rib);

/**
 * @brief 将指定路径节点标记为最优（置 BGP_ROUTE_FLAG_BEST 并移至链表首位）
 *
 * 同一 rthead 下其余路径的 BGP_ROUTE_FLAG_BEST 均被清除。
 *
 * @param rib        目标 RIB
 * @param nlri       NLRI 匹配键（用于定位 rthead）
 * @param best_route 待置为最优的路径节点（须属于该 rthead 的 route_list）
 */
void bgp_rib_mark_best(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, bgp_route_node_t *best_route);

/**
 * @brief 查找当前最优路径（只读）
 *
 * 最优路径须同时满足：位于 route_list 首位，且具有 BGP_ROUTE_FLAG_BEST + BGP_ROUTE_FLAG_VALID 标记。
 *
 * @param rib  目标 RIB
 * @param nlri NLRI 匹配键
 * @return 最优路径指针（借用，不可释放），未找到返回 NULL
 */
const bgp_route_node_t *bgp_rib_find_best(const bgp_rib_t *rib, const bgp_nlri_entry_t *nlri);

/**
 * @brief 遍历回调类型：对每条带 BGP_ROUTE_FLAG_BEST 的路径调用
 *
 * @param head      路径所属的前缀头（借用）
 * @param route     带 BGP_ROUTE_FLAG_BEST 的路径（借用）
 * @param user_data 调用方上下文指针
 */
typedef void (*bgp_rib_best_cb)(const bgp_rthead_t *head, const bgp_route_node_t *route, gpointer user_data);

/**
 * @brief 遍历 RIB 中所有带 BGP_ROUTE_FLAG_BEST 的路径，对每条调用回调
 *
 * @param rib       目标 RIB
 * @param cb        回调函数
 * @param user_data 传递给回调的上下文指针
 */
void bgp_rib_foreach_best(const bgp_rib_t *rib, bgp_rib_best_cb cb, gpointer user_data);

#endif /* BGP_RIB_H */
