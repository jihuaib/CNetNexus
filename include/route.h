/**
 * @file   route.h
 * @brief  Route 模块公共接口：协议常量、消息类型、IPC 载荷结构
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_H
#define ROUTE_H

#include <stdint.h>

#include "dev.h"

// ============================================================================
// 路由协议常量
// ============================================================================

/** 直连路由协议 */
#define ROUTE_PROTOCOL_CONNECTED 0u
/** 静态路由协议 */
#define ROUTE_PROTOCOL_STATIC 1u
/** BGP 路由协议 */
#define ROUTE_PROTOCOL_BGP 2u
/** OSPF 路由协议 */
#define ROUTE_PROTOCOL_OSPF 3u
/** 通配符：匹配所有协议（用于订阅/查询过滤） */
#define ROUTE_PROTOCOL_MAX 0xFFFFFFFFu

// ============================================================================
// VRF 常量
// ============================================================================

/** 默认 VRF ID */
#define ROUTE_VRF_DEFAULT 0u
/** 通配符：匹配所有 VRF（用于订阅/查询过滤） */
#define ROUTE_VRF_ALL 0xFFFFFFFFu

// ============================================================================
// AFI/SAFI 常量（与 BGP 保持一致）
// ============================================================================

/** IPv4 地址族 */
#define ROUTE_AFI_IPV4 1u
/** IPv6 地址族 */
#define ROUTE_AFI_IPV6 2u
/** 单播子地址族 */
#define ROUTE_SAFI_UNICAST 1u

// ============================================================================
// 管理距离（Administrative Distance）
// ============================================================================

/** 静态路由管理距离 */
#define ROUTE_ADMIN_DIST_STATIC 1
/** OSPF 路由管理距离 */
#define ROUTE_ADMIN_DIST_OSPF 110
/** BGP 路由管理距离 */
#define ROUTE_ADMIN_DIST_BGP 200

// ============================================================================
// IPC 消息类型
// ============================================================================

/** 订阅路由更新 */
#define ROUTE_MSG_TYPE_SUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0001)
/** 取消订阅 */
#define ROUTE_MSG_TYPE_UNSUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0002)
/** 全量路由上报（响应 SUBSCRIBE+FULL 标志） */
#define ROUTE_MSG_TYPE_REPORT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0003)
/** 增量路由更新（单条路由新增/撤销） */
#define ROUTE_MSG_TYPE_UPDATE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0004)
/** 通用应答 */
#define ROUTE_MSG_TYPE_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x00FF)

// ============================================================================
// 订阅标志
// ============================================================================

/** 订阅时请求全量路由快照 */
#define ROUTE_SUBSCRIBE_FLAG_FULL (1u << 0)

// ============================================================================
// IPC 载荷结构
// ============================================================================

#include "net_addr.h"

/**
 * @brief 订阅请求载荷
 */
typedef struct route_subscribe_req
{
    uint32_t protocol; /**< 订阅的路由协议（ROUTE_PROTOCOL_MAX = 全部） */
    uint32_t vrf_id;   /**< 订阅的 VRF ID（ROUTE_VRF_ALL = 全部） */
    uint32_t flags;    /**< 标志位（ROUTE_SUBSCRIBE_FLAG_FULL 等） */
} route_subscribe_req_t;

/**
 * @brief 单条路由条目（用于 REPORT/UPDATE 消息）
 */
typedef struct route_msg_entry
{
    uint32_t vrf_id;         /**< VRF ID */
    uint16_t afi;            /**< 地址族 */
    uint8_t safi;            /**< 子地址族 */
    uint8_t prefix_len;      /**< 前缀长度 */
    uint32_t protocol;       /**< 路由协议 */
    int32_t metric;          /**< 度量值 */
    int32_t preference;      /**< 管理距离（偏好值） */
    uint8_t is_withdraw;     /**< 1=撤销路由, 0=新增/更新路由 */
    uint8_t _pad[3];         /**< 对齐填充 */
    net_addr_t prefix_addr;  /**< 前缀地址（二进制） */
    net_addr_t nexthop_addr; /**< 下一跳地址（二进制） */
    net_addr_t source_addr;  /**< 路径来源标识（二进制 IP） */
} route_msg_entry_t;

/**
 * @brief 全量路由上报载荷（ROUTE_MSG_TYPE_REPORT）
 */
typedef struct route_msg_report
{
    uint32_t protocol;          /**< 上报的路由协议 */
    uint32_t route_count;       /**< 路由条目数量 */
    route_msg_entry_t routes[]; /**< 路由条目数组（flexible array member） */
} route_msg_report_t;

/**
 * @brief 通用应答载荷（ROUTE_MSG_TYPE_ACK）
 */
typedef struct route_msg_ack
{
    int32_t result; /**< 错误码（ERRCODE_SUCCESS = 0 表示成功） */
} route_msg_ack_t;

#endif /* ROUTE_H */
