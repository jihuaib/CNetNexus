/**
 * @file   route_os.h
 * @brief  OS 内核路由下发接口（通过 Netlink 安装/撤销 Linux 路由表条目）
 * @author jhb
 * @date   2026/03/22
 */
#ifndef ROUTE_OS_H
#define ROUTE_OS_H

#include <glib.h>
#include <netinet/in.h>

#include "route.h"

/**
 * @brief 向 Linux 内核安装一条路由（RTM_NEWROUTE）
 *
 * 根据 entry->protocol 自动选择路由类型：
 *   - ROUTE_PROTOCOL_BLACKHOLE → RTN_BLACKHOLE
 *   - ROUTE_PROTOCOL_CONNECTED (out_ifindex != 0) → RTN_UNICAST + RTA_OIF
 *   - 其他（静态/BGP 等，nexthop 非零）→ RTN_UNICAST + RTA_GATEWAY
 *
 * @param entry 路由条目（包含 prefix_addr、prefix_len、protocol、out_ifindex、nexthop_addr）
 * @return 成功返回 0，失败返回 -1
 */
int route_os_install(const route_msg_entry_t *entry);

/**
 * @brief 从 Linux 内核撤销一条路由（RTM_DELROUTE）
 * @param entry 路由条目
 * @return 成功返回 0，失败返回 -1（路由不存在时忽略 ESRCH 错误）
 */
int route_os_withdraw(const route_msg_entry_t *entry);

/**
 * @brief 通过 Netlink RTM_GETROUTE dump 读取 Linux 内核路由表并格式化输出
 * @param buf    输出缓冲区（不可为 NULL）
 * @param family 地址族（AF_INET 或 AF_INET6）
 * @return 成功返回 0，失败返回 -1
 */
int route_os_show(GString *buf, sa_family_t family);

#endif /* ROUTE_OS_H */
