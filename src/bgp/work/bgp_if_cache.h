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
