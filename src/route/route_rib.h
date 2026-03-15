/**
 * @file   route_rib.h
 * @brief  Route 模块内存 RIB：前缀头 + 多协议多路径存储（二进制键，无字符串拼接）
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_RIB_H
#define ROUTE_RIB_H

#include <glib.h>
#include <stdint.h>

#include "net_addr.h"

// ============================================================================
// 复合键类型定义
// ============================================================================

/**
 * @brief 前缀头复合键（内嵌于 route_head_t，直接作为 GTree 键指针）
 *        使用 memcmp 进行二进制有序比较，创建时须 memset 为 0
 */
typedef struct route_head_key
{
    uint32_t vrf_id;    /**< VRF ID */
    uint16_t afi;       /**< 地址族 */
    uint8_t prefix_len; /**< 前缀长度 */
    uint8_t _pad;       /**< 填充对齐 */
    net_addr_t addr;    /**< 前缀地址（二进制） */
} route_head_key_t;

/**
 * @brief 路径复合键（内嵌于 route_path_t，直接作为 GHashTable 键指针）
 *        使用 protocol + net_addr_equal 进行 hash/equal 比较，创建时须 memset 为 0
 */
typedef struct route_path_key
{
    uint32_t protocol; /**< 路由协议 */
    uint8_t _pad[4];   /**< 填充对齐 */
    net_addr_t source; /**< 路径来源（静态路由用 nexthop，BGP 用邻居 IP） */
} route_path_key_t;

// ============================================================================
// 数据结构定义
// ============================================================================

/**
 * @brief 单条路径（同一前缀下按协议 + 来源区分）
 */
typedef struct route_path
{
    route_path_key_t key;   /**< 内嵌键（GHashTable 键指向 &path->key） */
    net_addr_t nexthop;     /**< 下一跳地址（二进制） */
    int32_t metric;         /**< 度量值 */
    int32_t preference;     /**< 管理距离 */
    gint64 updated_at_usec; /**< 最近更新时间（g_get_real_time） */
} route_path_t;

/**
 * @brief 路由头（Route Head）：表示一个唯一前缀
 */
typedef struct route_head
{
    route_head_key_t key;  /**< 内嵌键（GTree 键指向 &head->key） */
    GHashTable *path_hash; /**< route_path_key_t* -> route_path_t*（key 内嵌于 value） */
} route_head_t;

/**
 * @brief 路由 RIB（内存路由信息库）
 */
typedef struct route_rib
{
    GTree *head_tree;    /**< route_head_key_t* -> route_head_t*（key 内嵌于 value） */
    uint32_t head_count; /**< 前缀头总数 */
    uint32_t path_count; /**< 路径总数（所有前缀下累计） */
} route_rib_t;

/**
 * @brief 路径遍历回调函数类型
 * @param head 前缀头
 * @param path 路径
 * @param userdata 用户数据
 */
typedef void (*route_path_cb)(const route_head_t *head, const route_path_t *path, void *userdata);

// ============================================================================
// RIB 生命周期
// ============================================================================

/**
 * @brief 创建 RIB
 * @return 新分配的 RIB，失败返回 NULL
 */
route_rib_t *route_rib_create(void);

/**
 * @brief 销毁 RIB（释放所有内存）
 * @param rib 目标 RIB
 */
void route_rib_destroy(route_rib_t *rib);

// ============================================================================
// RIB 写操作
// ============================================================================

/**
 * @brief 向 RIB 添加或更新一条路径
 * @param rib         目标 RIB
 * @param vrf_id      VRF ID
 * @param afi         地址族
 * @param prefix_addr 前缀地址（二进制）
 * @param prefix_len  前缀长度
 * @param protocol    路由协议
 * @param source      路径来源地址（二进制）
 * @param nexthop     下一跳地址（二进制）
 * @param metric      度量值
 * @param preference  管理距离
 * @return 1=新增路径, 0=更新已有路径, -1=失败
 */
int route_rib_add(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                  uint32_t protocol, const net_addr_t *source, const net_addr_t *nexthop, int32_t metric,
                  int32_t preference);

/**
 * @brief 从 RIB 删除一条路径（可选回调，在删除前触发）
 * @param rib         目标 RIB
 * @param vrf_id      VRF ID
 * @param afi         地址族
 * @param prefix_addr 前缀地址（二进制）
 * @param prefix_len  前缀长度
 * @param protocol    路由协议
 * @param source      路径来源地址（二进制）
 * @param cb          删除前回调（可为 NULL）
 * @param userdata    回调用户数据
 * @return 1=删除成功, 0=未命中, -1=失败
 */
int route_rib_del(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                  uint32_t protocol, const net_addr_t *source, route_path_cb cb, void *userdata);

/**
 * @brief 删除某前缀下指定协议的所有路径（批量删除，可选回调）
 * @param rib         目标 RIB
 * @param vrf_id      VRF ID
 * @param afi         地址族
 * @param prefix_addr 前缀地址（二进制）
 * @param prefix_len  前缀长度
 * @param protocol    路由协议
 * @param cb          每条删除前回调（可为 NULL）
 * @param userdata    回调用户数据
 * @return 删除路径数，-1=失败
 */
int route_rib_del_proto_for_prefix(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr,
                                   uint8_t prefix_len, uint32_t protocol, route_path_cb cb, void *userdata);

// ============================================================================
// RIB 查询
// ============================================================================

/**
 * @brief 查找前缀头（只读）
 * @param rib         目标 RIB
 * @param vrf_id      VRF ID
 * @param afi         地址族
 * @param prefix_addr 前缀地址（二进制）
 * @param prefix_len  前缀长度
 * @return 前缀头指针（不可修改），未找到返回 NULL
 */
const route_head_t *route_rib_lookup_head(const route_rib_t *rib, uint32_t vrf_id, uint16_t afi,
                                          const net_addr_t *prefix_addr, uint8_t prefix_len);

/**
 * @brief 在前缀头下按协议和来源查找路径（只读）
 * @param head     前缀头
 * @param protocol 路由协议
 * @param source   路径来源地址（二进制）
 * @return 路径指针（不可修改），未找到返回 NULL
 */
const route_path_t *route_rib_lookup_path(const route_head_t *head, uint32_t protocol, const net_addr_t *source);

/**
 * @brief 遍历 RIB 中的所有路径
 * @param rib          目标 RIB
 * @param proto_filter 协议过滤（ROUTE_PROTOCOL_MAX = 全部）
 * @param vrf_filter   VRF 过滤（ROUTE_VRF_ALL = 全部）
 * @param cb           回调函数
 * @param userdata     回调用户数据
 */
void route_rib_walk(route_rib_t *rib, uint32_t proto_filter, uint32_t vrf_filter, route_path_cb cb, void *userdata);

// ============================================================================
// 统计
// ============================================================================

/**
 * @brief 获取前缀头总数
 */
uint32_t route_rib_head_count(const route_rib_t *rib);

/**
 * @brief 获取路径总数
 */
uint32_t route_rib_path_count(const route_rib_t *rib);

#endif /* ROUTE_RIB_H */
