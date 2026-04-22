/**
 * @file   if_link_monitor.h
 * @brief  Netlink 链路状态监听线程（RTM_NEWLINK / RTM_DELLINK）
 * @author jhb
 * @date   2026/04/20
 */
#ifndef IF_LINK_MONITOR_H
#define IF_LINK_MONITOR_H

#include "work/if_worker.h"

/**
 * @brief 启动 Netlink 链路监听线程
 *
 * 创建 AF_NETLINK 套接字并绑定 RTMGRP_LINK 组播，在独立线程中监听
 * RTM_NEWLINK / RTM_DELLINK 事件。当检测到已管理接口（在 interface_map 中）
 * 被销毁或重建时，自动刷新 ifindex、重新下发 IP 地址和直连路由。
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

#endif /* IF_LINK_MONITOR_H */
