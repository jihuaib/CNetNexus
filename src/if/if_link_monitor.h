/**
 * @file   if_link_monitor.h
 * @brief  Netlink 接口状态监听线程（link + IPv6 addr）
 * @author jhb
 * @date   2026/04/20
 */
#ifndef IF_LINK_MONITOR_H
#define IF_LINK_MONITOR_H

#include "work/if_worker.h"

/**
 * @brief 启动 Netlink 链路监听线程
 *
 * 创建 AF_NETLINK 套接字并绑定 RTMGRP_LINK / RTMGRP_IPV6_IFADDR 组播，
 * 在独立线程中监听 RTM_NEWLINK / RTM_DELLINK / RTM_NEWADDR / RTM_DELADDR。
 * 当检测到已管理接口被销毁、重建或 IPv6 link-local 地址变化时，
 * 将运行态状态同步到 IF worker。
 *
 * @return 0 成功，-1 失败
 */
int if_link_monitor_start(void);

/**
 * @brief 停止 Netlink 链路监听线程并释放资源
 */
void if_link_monitor_stop(void);

/**
 * @brief IF work 线程消费链路监控事件
 */
void if_link_monitor_handle_work_event(const if_work_link_event_t *evt);

/**
 * @brief IF work 线程消费地址监控事件
 */
void if_link_monitor_handle_addr_work_event(const if_work_addr_event_t *evt);

#endif /* IF_LINK_MONITOR_H */
