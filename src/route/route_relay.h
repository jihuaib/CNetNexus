/**
 * @file   route_relay.h
 * @brief  Route nexthop 迭代 relay（注册/回调）
 */
#ifndef ROUTE_RELAY_H
#define ROUTE_RELAY_H

#include <glib.h>

#include "dev.h"
#include "net_addr.h"

/**
 * @brief 全量重算已注册 nexthop 的可达性，并按变化回推 owner 模块
 */
void route_recompute_iter_paths(void);

/**
 * @brief 处理 nexthop 注册消息（ROUTE_MSG_TYPE_NH_REGISTER）
 */
void route_relay_handle_nh_register(dev_ipc_message_t *msg);

/**
 * @brief 处理 nexthop 取消注册消息（ROUTE_MSG_TYPE_NH_UNREGISTER）
 */
void route_relay_handle_nh_unregister(dev_ipc_message_t *msg);

/**
 * @brief 格式化输出 nexthop watch 表到缓冲区
 * @param buf          输出 GString 缓冲区
 * @param module_filter 按 owner_module_id 过滤（0 表示不过滤）
 * @param has_filter   非零表示启用 module_filter 过滤
 * @param afi_filter   地址族过滤（ROUTE_AFI_IPV4/IPV6）
 * @param has_afi_filter 非零表示启用 afi_filter 过滤
 */
void route_relay_show(GString *buf, uint32_t module_filter, int has_filter, uint16_t afi_filter, int has_afi_filter);

/**
 * @brief 清理 relay 内部状态
 */
void route_relay_cleanup(void);

/**
 * @brief 直接将 nexthop 注册到 relay watch 表（供模块内部使用，不经过 IPC）
 *
 * 若同键条目已存在则更新；创建后立即计算当前可达性并返回结果。
 *
 * @param vrf_id         VRF ID
 * @param afi            地址族
 * @param nexthop_addr   下一跳地址（二进制）
 * @param owner_module_id 注册方模块 ID
 * @return 1=当前可达，0=当前不可达
 */
int route_relay_register_direct(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr,
                                uint32_t owner_module_id);

/**
 * @brief 直接从 relay watch 表移除 nexthop（供模块内部使用，不经过 IPC）
 *
 * @param vrf_id         VRF ID
 * @param afi            地址族
 * @param nexthop_addr   下一跳地址（二进制）
 * @param owner_module_id 注册方模块 ID
 */
void route_relay_unregister_direct(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr,
                                   uint32_t owner_module_id);

/**
 * @brief 检查指定 nexthop 是否在 RIB 中可达（递归迭代，最大深度 ROUTE_ITER_NH_MAX_DEPTH）
 *
 * 供模块内部（route_static.c）直接调用，避免通过 IPC 绕路。
 *
 * @param vrf_id       VRF ID
 * @param afi          地址族（ROUTE_AFI_IPV4 或 ROUTE_AFI_IPV6）
 * @param nexthop_addr 下一跳地址（二进制）
 * @return 1=可达，0=不可达
 */
int route_relay_nh_is_resolved(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr);

/**
 * @brief 沿 nexthop 迭代链查找最终出接口索引
 *
 * 与 route_relay_nh_is_resolved 逻辑相同，但返回链末端直连路径的 out_ifindex。
 * 用于在静态路由写入 RIB 时填充出接口信息。
 *
 * @param vrf_id       VRF ID
 * @param afi          地址族
 * @param nexthop_addr 下一跳地址（二进制）
 * @return 出接口索引（0 表示未找到或不可达）
 */
uint32_t route_relay_nh_get_ifindex(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr);

/**
 * @brief 递归解析下一跳，返回可直接写入内核路由的网关地址与出接口
 *
 * 解析逻辑沿 RIB 的最优覆盖路径迭代，直到命中直连路径（CONNECTED）。
 * 返回的 gateway_out 为“最终直连网段上的下一跳地址”，可用于 RTA_GATEWAY。
 *
 * @param vrf_id          VRF ID
 * @param afi             地址族
 * @param nexthop_addr    原始下一跳地址（二进制）
 * @param gateway_out     解析后的可直连网关地址（输出，不能为空）
 * @param out_ifindex_out 解析出的出接口索引（输出，可为 NULL）
 * @return 1=解析成功，0=解析失败
 */
int route_relay_nh_resolve_gateway(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr,
                                   net_addr_t *gateway_out, uint32_t *out_ifindex_out);

#endif /* ROUTE_RELAY_H */
