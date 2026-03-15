/**
 * @file   if_event.h
 * @brief  IF 模块事件订阅公共接口
 * @author jhb
 * @date   2026/03/15
 */
#ifndef IF_EVENT_H
#define IF_EVENT_H

#include <net/if.h>
#include <stdint.h>

#include "dev.h"

// ============================================================================
// 接口类型位图
// ============================================================================

/** Ethernet 接口（当前仅支持） */
#define IF_INTF_TYPE_ETH (1u << 0)
/** 通配：匹配所有接口类型 */
#define IF_INTF_TYPE_ALL 0xFFFFFFFFu

// ============================================================================
// 接口事件位图
// ============================================================================

/** 接口 UP 事件 */
#define IF_EVENT_UP (1u << 0)
/** 接口 DOWN 事件 */
#define IF_EVENT_DOWN (1u << 1)
/** 通配：匹配所有事件 */
#define IF_EVENT_ALL 0xFFFFFFFFu

// ============================================================================
// IPC 消息类型
// ============================================================================

/** 订阅 IF 事件 */
#define IF_MSG_TYPE_SUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_IF, 0x0001)
/** 取消订阅 IF 事件 */
#define IF_MSG_TYPE_UNSUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_IF, 0x0002)
/** IF 事件通知 */
#define IF_MSG_TYPE_EVENT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_IF, 0x0003)
/** IF 通用 ACK */
#define IF_MSG_TYPE_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_IF, 0x00FF)

// ============================================================================
// 常量
// ============================================================================

/** 逻辑接口名最大长度（与 if_map.h 的 LOGICAL_NAME_LEN 保持一致） */
#define IF_LOGICAL_NAME_MAX 32

// ============================================================================
// IPC 载荷结构
// ============================================================================

/**
 * @brief IF 事件订阅请求载荷
 */
typedef struct if_subscribe_req
{
    uint32_t if_type_mask; /**< 接口类型位图（IF_INTF_TYPE_*） */
    uint32_t event_mask;   /**< 事件位图（IF_EVENT_*） */
    uint32_t flags;        /**< 预留标志位（当前未使用） */
} if_subscribe_req_t;

/**
 * @brief IF 事件消息载荷
 */
typedef struct if_event_msg
{
    uint32_t if_type;                       /**< 接口类型（单值，对应 IF_INTF_TYPE_* 某一位） */
    uint32_t event;                         /**< 事件类型（单值，对应 IF_EVENT_* 某一位） */
    uint8_t admin_up;                       /**< 1=up, 0=down */
    uint8_t _pad[3];                        /**< 对齐填充 */
    char logical_name[IF_LOGICAL_NAME_MAX]; /**< 逻辑接口名（如 GE-1） */
    char physical_name[IFNAMSIZ];           /**< 物理接口名（如 eth0） */
} if_event_msg_t;

/**
 * @brief IF 通用 ACK 载荷
 */
typedef struct if_msg_ack
{
    int32_t result; /**< ERRCODE_SUCCESS=0 表示成功 */
} if_msg_ack_t;

#endif /* IF_EVENT_H */
