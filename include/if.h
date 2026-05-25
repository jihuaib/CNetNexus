/**
 * @file   if.h
 * @brief  IF 模块公共接口：事件订阅、IPC 载荷、缓存 API
 * @author jhb
 * @date   2026/03/15
 *
 * 本头文件合并了原 if_event.h 和 if_api.h 的全部内容。
 */
#ifndef IF_H
#define IF_H

#include <glib.h>
#include <net/if.h>
#include <stdint.h>
#include <sys/socket.h>

#include "dev.h"
#include "net_addr.h"

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

/** 物理链路建立事件 */
#define IF_EVENT_LINK_UP (1u << 0)
/** 物理链路断开事件 */
#define IF_EVENT_LINK_DOWN (1u << 1)
/** 协议层就绪事件（配置了 IP 且链路 UP、未 Shutdown） */
#define IF_EVENT_PROTO_UP (1u << 2)
/** 协议层断开事件（IP 删除、或者链路 DOWN、或者被 Shutdown） */
#define IF_EVENT_PROTO_DOWN (1u << 3)
/** 接口 VRF 绑定变化事件 */
#define IF_EVENT_VRF_CHANGE (1u << 4)
/** 平滑同步起始：IF replay 即将开始（订阅方据此清空 IF 缓存） */
#define IF_EVENT_SMOOTHSTART (1u << 5)
/** 平滑同步结束：IF replay 已完成（订阅方据此触发 IF 相关的 db_restore） */
#define IF_EVENT_SMOOTHEND (1u << 6)
/** 通配：匹配所有事件 */
#define IF_EVENT_ALL 0xFFFFFFFFu

/** 地址事件标志：IPv6 链路本地地址 */
#define IF_ADDR_FLAG_LINK_LOCAL (1u << 0)

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
/** VRF 名称最大长度（与 VRF 模块保持一致，避免 IF 公共头依赖 vrf.h） */
#define IF_VRF_NAME_MAX 64

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
    uint8_t link_up;                        /**< 1=up, 0=down (Physical Link State) */
    uint8_t _pad[3];                        /**< 对齐填充 */
    char logical_name[IF_LOGICAL_NAME_MAX]; /**< 逻辑接口名（如 GE-1） */
    char physical_name[IFNAMSIZ];           /**< 物理接口名（如 eth0） */
    char vrf_name[IF_VRF_NAME_MAX];         /**< 接口所属 VRF 名称，空=public */
} if_event_msg_t;

/**
 * @brief IF 协议就绪事件消息载荷（IF_EVENT_PROTO_UP / IF_EVENT_PROTO_DOWN 专用）
 */
typedef struct if_addr_event_msg
{
    uint32_t if_type;                       /**< 接口类型（单值，对应 IF_INTF_TYPE_* 某一位） */
    uint32_t event;                         /**< 事件类型（IF_EVENT_PROTO_UP 或 IF_EVENT_PROTO_DOWN） */
    char logical_name[IF_LOGICAL_NAME_MAX]; /**< 逻辑接口名（如 GE-1） */
    char physical_name[IFNAMSIZ];           /**< 物理接口名（如 eth0） */
    char vrf_name[IF_VRF_NAME_MAX];         /**< 接口所属 VRF 名称，空=public */
    uint16_t afi;                           /**< 地址族（ROUTE_AFI_IPV4 / ROUTE_AFI_IPV6） */
    uint8_t prefix_len;                     /**< 前缀长度 */
    uint8_t addr_flags;                     /**< 地址标志（IF_ADDR_FLAG_*） */
    net_addr_t addr;                        /**< 接口地址 */
    uint32_t ifindex;                       /**< 接口索引 */
} if_addr_event_msg_t;

/**
 * @brief IF 通用 ACK 载荷
 */
typedef struct if_msg_ack
{
    int32_t result; /**< ERRCODE_SUCCESS=0 表示成功 */
} if_msg_ack_t;

/**
 * @brief 接口映射表单条目（ifindex → 逻辑名）
 */
typedef struct if_intf_map_item
{
    uint32_t ifindex;                       /**< Linux 接口索引（0 表示虚拟/未知） */
    char logical_name[IF_LOGICAL_NAME_MAX]; /**< 逻辑接口名（如 GE-1、loop1） */
} if_intf_map_item_t;

/**
 * @brief IF_MSG_TYPE_GET_INTF_MAP 响应载荷
 *
 * 载荷布局：固定头部 + items[] 变长数组
 * 总长度 = sizeof(if_intf_map_resp_t) + (count - 1) * sizeof(if_intf_map_item_t)
 */
typedef struct if_intf_map_resp
{
    int32_t result;              /**< ERRCODE_SUCCESS=0 表示成功 */
    uint32_t count;              /**< items 数组元素数量 */
    if_intf_map_item_t items[1]; /**< 变长条目数组（实际长度由 count 决定） */
} if_intf_map_resp_t;

// ============================================================================
// IF 缓存 API（原 if_api.h）
// ============================================================================

/**
 * @brief IF 缓存条目（按逻辑接口聚合）
 */
typedef struct if_api_cache_entry
{
    char logical_name[IF_LOGICAL_NAME_MAX]; /**< 逻辑接口名（如 GE-1） */
    char physical_name[IFNAMSIZ];           /**< 物理接口名（如 eth0） */
    uint32_t ifindex;                       /**< 接口索引 */
    char vrf_name[IF_VRF_NAME_MAX];         /**< 接口所属 VRF 名称，空=public */
    uint8_t link_up;                        /**< 1=物理链路连接, 0=物理链路断开 */
    uint8_t proto_up;                       /**< 1=协议就绪(有IP且不down), 0=未就绪 */
    uint8_t _pad[2];                        /**< 对齐填充 */
    net_addr_t ipv4_addr;                   /**< IPv4 地址（family=0 表示未配置） */
    uint8_t ipv4_prefix_len;                /**< IPv4 前缀长度 */
    net_addr_t ipv6_addr;                   /**< IPv6 地址（family=0 表示未配置） */
    uint8_t ipv6_prefix_len;                /**< IPv6 前缀长度 */
    net_addr_t ipv6_linklocal_addr;         /**< IPv6 link-local 地址（family=0 表示未知） */
    uint8_t ipv6_linklocal_prefix_len;      /**< IPv6 link-local 前缀长度 */
} if_api_cache_entry_t;

/**
 * @brief IF 缓存遍历回调
 * @return TRUE 表示停止遍历，FALSE 继续
 */
typedef gboolean (*if_api_cache_iter_fn)(const if_api_cache_entry_t *entry, void *user_data);

/**
 * @brief 初始化 IF 缓存
 */
void if_api_cache_init(void);

/**
 * @brief 清理 IF 缓存
 */
void if_api_cache_cleanup(void);

/**
 * @brief 用 IF 事件更新缓存
 */
void if_api_cache_on_event(const dev_ipc_message_t *msg);

/**
 * @brief 按逻辑接口名查询缓存条目
 *
 * 返回的指针由库内部持有，仅在当前缓存版本下有效。
 */
const if_api_cache_entry_t *if_api_cache_lookup(const char *logical_name);

/**
 * @brief 遍历所有缓存条目
 */
void if_api_cache_foreach(if_api_cache_iter_fn iter_fn, void *user_data);

/**
 * @brief 按逻辑接口名查询 ifindex
 */
uint32_t if_api_cache_get_ifindex(const char *logical_name);

/**
 * @brief 按 ifindex 查询逻辑接口名
 */
const char *if_api_cache_get_logical_name(uint32_t ifindex);

/**
 * @brief 外部提示逻辑接口与 ifindex 映射
 */
void if_api_cache_hint_ifindex(const char *logical_name, uint32_t ifindex);

/**
 * @brief 向 IF 模块发送订阅请求
 * @return ERRCODE_SUCCESS/ERRCODE_FAIL
 */
int if_api_subscribe(dev_ipc_context_t *ctx, uint32_t if_type_mask, uint32_t event_mask, uint32_t flags);

/**
 * @brief 订阅 IF 全量事件（ALL type + ALL event）
 */
int if_api_subscribe_all(dev_ipc_context_t *ctx);

#endif /* IF_H */
