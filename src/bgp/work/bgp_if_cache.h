/**
 * @file   bgp_if_cache.h
 * @brief  BGP 本地接口缓存：通过 IF 事件订阅维护接口地址/状态，避免跨模块 DB 查询
 * @author jhb
 * @date   2026/04/07
 */
#ifndef BGP_IF_CACHE_H
#define BGP_IF_CACHE_H

#include <glib.h>
#include <stdint.h>
#include <sys/socket.h>

#include "dev.h"
#include "if_event.h"
#include "net_addr.h"

/**
 * @brief 本地缓存的接口条目
 */
typedef struct bgp_if_cache_entry
{
    char logical_name[IF_LOGICAL_NAME_MAX]; /**< 逻辑接口名（如 GE-1） */
    uint8_t admin_up;                       /**< 1=up, 0=down */
    uint8_t _pad[3];                        /**< 对齐填充 */
    net_addr_t ipv4_addr;                   /**< IPv4 地址（family=0 表示未配置） */
    uint8_t ipv4_prefix_len;                /**< IPv4 前缀长度 */
    net_addr_t ipv6_addr;                   /**< IPv6 地址（family=0 表示未配置） */
    uint8_t ipv6_prefix_len;                /**< IPv6 前缀长度 */
} bgp_if_cache_entry_t;

/**
 * @brief 初始化 BGP 本地接口缓存（worker 线程调用）
 */
void bgp_if_cache_init(void);

/**
 * @brief 清理 BGP 本地接口缓存（shutdown 时调用）
 */
void bgp_if_cache_cleanup(void);

/**
 * @brief 处理 IF 事件消息，更新本地接口缓存
 *
 * 根据 payload 大小自动区分 if_event_msg_t（UP/DOWN）和 if_addr_event_msg_t（ADDR_ADD/DEL）。
 * 仅在 worker 线程中调用。
 *
 * @param msg IF 事件 IPC 消息
 */
void bgp_if_cache_on_event(dev_ipc_message_t *msg);

/**
 * @brief 判断邻居地址是否直连（替代原 db_rpc_query("if_interface") 全表扫描）
 *
 * 遍历本地缓存中所有非 shutdown 接口，检查邻居地址是否在某接口子网内。
 *
 * @param neighbor_addr 邻居地址
 * @return TRUE=直连，FALSE=非直连
 */
gboolean bgp_if_cache_is_directly_connected(const net_addr_t *neighbor_addr);

/**
 * @brief 按接口名解析源地址（替代原 db_rpc_query("if_interface") 按名查询）
 *
 * @param if_name      逻辑接口名
 * @param peer_family  对端地址族（AF_INET/AF_INET6，0 表示自动回退）
 * @param out_addr     输出：解析到的地址
 * @param errmsg       输出：失败时的错误描述
 * @param errmsg_len   errmsg 缓冲区长度
 * @return 0 成功，-1 失败
 */
int bgp_if_cache_resolve_source_addr(const char *if_name, sa_family_t peer_family, net_addr_t *out_addr, char *errmsg,
                                     size_t errmsg_len);

#endif /* BGP_IF_CACHE_H */
