/**
 * @file   route_cfg_apply.h
 * @brief  Route 配置内存态应用接口（仅在 worker 线程调用）
 * @author jhb
 * @date   2026/03/28
 */
#ifndef ROUTE_CFG_APPLY_H
#define ROUTE_CFG_APPLY_H

#include <stdint.h>

#include "if.h"
#include "net_addr.h"

// ============================================================================
// 应用命令
// ============================================================================

/**
 * @brief 配置应用操作类型
 */
typedef enum route_apply_op
{
    ROUTE_APPLY_STATIC_ADD = 1,        /**< 添加静态路由 */
    ROUTE_APPLY_STATIC_DEL = 2,        /**< 删除静态路由（精确匹配 nexthop） */
    ROUTE_APPLY_STATIC_DEL_PREFIX = 3, /**< 删除某前缀的所有静态路由 */
    ROUTE_APPLY_BATCH_ADD = 4,         /**< 添加 batch 路由 */
    ROUTE_APPLY_BATCH_DEL = 5,         /**< 删除 batch 路由 */
} route_apply_op_t;

/**
 * @brief 配置应用命令结构体（栈分配，跨线程传递）
 *
 * IPC 线程填写 op 和对应联合体分支，调用 route_worker_dispatch_apply() 同步等待。
 * worker 线程执行后将结果写入 rc 字段。
 */
typedef struct route_apply_cmd
{
    route_apply_op_t op; /**< 操作类型 */

    union
    {
        /** ROUTE_APPLY_STATIC_ADD */
        struct
        {
            uint32_t vrf_id;                      /**< VRF ID */
            uint16_t afi;                         /**< 地址族 */
            uint8_t prefix_len;                   /**< 前缀长度 */
            net_addr_t prefix_addr;               /**< 前缀地址（二进制） */
            net_addr_t nexthop_addr;              /**< 下一跳地址（二进制，interface-only 时全零） */
            int32_t metric;                       /**< 度量值 */
            int32_t preference;                   /**< 管理距离 */
            char out_ifname[IF_LOGICAL_NAME_MAX]; /**< 出接口逻辑名（空字符串=不约束） */
        } static_add;

        /** ROUTE_APPLY_STATIC_DEL */
        struct
        {
            uint32_t vrf_id;                      /**< VRF ID */
            uint16_t afi;                         /**< 地址族 */
            uint8_t prefix_len;                   /**< 前缀长度 */
            net_addr_t prefix_addr;               /**< 前缀地址（二进制） */
            net_addr_t nexthop_addr;              /**< 下一跳地址（二进制，interface-only 时全零） */
            char out_ifname[IF_LOGICAL_NAME_MAX]; /**< 出接口逻辑名（空字符串=不约束） */
        } static_del;

        /** ROUTE_APPLY_STATIC_DEL_PREFIX */
        struct
        {
            uint32_t vrf_id;        /**< VRF ID */
            uint16_t afi;           /**< 地址族 */
            uint8_t prefix_len;     /**< 前缀长度 */
            net_addr_t prefix_addr; /**< 前缀地址（二进制） */
        } static_del_prefix;

        /** ROUTE_APPLY_BATCH_ADD */
        struct
        {
            char name[64];       /**< batch 名称 */
            uint16_t afi;        /**< 地址族 */
            uint8_t prefix_len;  /**< 前缀长度 */
            char start_addr[64]; /**< 起始地址字符串 */
            int64_t count;       /**< 路由数量 */
            char nexthop[64];    /**< 下一跳地址字符串 */
        } batch_add;

        /** ROUTE_APPLY_BATCH_DEL */
        struct
        {
            char name[64]; /**< batch 名称 */
        } batch_del;
    } u;

    int rc; /**< 执行结果（worker 线程回填：>=0 成功，-1 失败） */
} route_apply_cmd_t;

// ============================================================================
// 应用函数（仅在 worker 线程调用）
// ============================================================================

/**
 * @brief 应用静态路由配置（ADD / DEL / DEL_PREFIX）到内存状态
 *
 * 处理 ROUTE_APPLY_STATIC_ADD / ROUTE_APPLY_STATIC_DEL / ROUTE_APPLY_STATIC_DEL_PREFIX
 * 操作，完成候选表更新、RIB 写入/撤销、订阅者通知，并将结果写入 apply->rc。
 *
 * @param apply 配置应用命令（由 worker 线程处理）
 */
void route_cfg_apply_static(route_apply_cmd_t *apply);

/**
 * @brief 应用 batch 路由配置（ADD / DEL）到内存状态
 *
 * 处理 ROUTE_APPLY_BATCH_ADD / ROUTE_APPLY_BATCH_DEL 操作，
 * 生成/删除批量路由，更新 batch_entries 列表，并将结果写入 apply->rc。
 *
 * @param apply 配置应用命令（由 worker 线程处理）
 */
void route_cfg_apply_batch(route_apply_cmd_t *apply);

#endif /* ROUTE_CFG_APPLY_H */
