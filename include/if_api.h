/**
 * @file   if_api.h
 * @brief  IF 模块缓存 API：统一接口状态缓存、事件更新与订阅辅助
 * @author jhb
 * @date   2026/04/11
 */
#ifndef IF_API_H
#define IF_API_H

#include <glib.h>
#include <net/if.h>
#include <stdint.h>
#include <sys/socket.h>

#include "dev.h"
#include "if_event.h"
#include "net_addr.h"

/**
 * @brief IF 缓存条目（按逻辑接口聚合）
 */
typedef struct if_api_cache_entry
{
    char logical_name[IF_LOGICAL_NAME_MAX]; /**< 逻辑接口名（如 GE-1） */
    char physical_name[IFNAMSIZ];           /**< 物理接口名（如 eth0） */
    uint32_t ifindex;                       /**< 接口索引 */
    uint8_t link_up;                        /**< 1=物理链路连接, 0=物理链路断开 */
    uint8_t proto_up;                       /**< 1=协议就绪(有IP且不down), 0=未就绪 */
    uint8_t _pad[2];                        /**< 对齐填充 */
    net_addr_t ipv4_addr;                   /**< IPv4 地址（family=0 表示未配置） */
    uint8_t ipv4_prefix_len;                /**< IPv4 前缀长度 */
    net_addr_t ipv6_addr;                   /**< IPv6 地址（family=0 表示未配置） */
    uint8_t ipv6_prefix_len;                /**< IPv6 前缀长度 */
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

#endif /* IF_API_H */
