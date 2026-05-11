/**
 * @file   ldp_discovery.h
 * @brief  LDP basic discovery：UDP 646 + 224.0.0.2 hello
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_DISCOVERY_H
#define LDP_DISCOVERY_H

#include "ldp_worker.h"

/** LDP basic discovery 多播组 */
#define LDP_MCAST_GROUP_V4 "224.0.0.2"
/** 接口未知 ifindex 时的占位 */
#define LDP_IFINDEX_UNKNOWN 0u

/**
 * @brief 把接口加入 hello 工作集，必要时建立 UDP socket、加入多播
 *
 * 接口已存在时仅更新参数。接口未配置 IP 或 ifindex 未知时仅记录配置，
 * 等待 IF 事件刷新缓存后再尝试建立 socket。
 */
int ldp_discovery_iface_set(const ldp_if_cfg_t *cfg);

/**
 * @brief 移除接口的 hello 状态，关闭 socket 与所有相关 adjacency
 */
int ldp_discovery_iface_del(const char *ifname);

/**
 * @brief 全局协议参数变化时通知（admin/lsr_id 变化触发重发 hello、清空 adj）
 */
void ldp_discovery_proto_changed(uint8_t admin_changed, uint8_t lsr_id_changed);

/**
 * @brief 接口事件触发（IF cache 已更新）：刷新 ifindex/IP 并按需建立 socket
 */
void ldp_discovery_on_if_event(const char *ifname);

/**
 * @brief 1Hz tick：发 hello、过期 adjacency
 */
void ldp_discovery_tick(void);

/**
 * @brief epoll 上 udp_fd 可读事件
 */
void ldp_discovery_handle_rx(ldp_iface_state_t *iface);

#endif /* LDP_DISCOVERY_H */
