/**
 * @file   vrf_os.h
 * @brief  VRF L3VRF 设备下发（netlink RTM_NEWLINK / DELLINK type=vrf）
 * @author jhb
 * @date   2026/05/02
 */
#ifndef VRF_OS_H
#define VRF_OS_H

#include <stdint.h>

/**
 * @brief 创建 L3VRF 设备并 set up
 *
 * 等价于：
 *   ip link add <name> type vrf table <table_id>
 *   ip link set <name> up
 *
 * @param name      设备名（如 "vrf-blue"）
 * @param table_id  L3VRF 路由表 ID
 * @return 0 成功，-1 失败；已存在视为成功
 */
int vrf_os_install(const char *name, uint32_t table_id);

/**
 * @brief 删除 L3VRF 设备
 * @return 0 成功，-1 失败；不存在视为成功
 */
int vrf_os_remove(const char *name);

#include <glib.h>

/**
 * @brief 通过 RTM_GETLINK dump 列出内核中所有 type=vrf 的设备并格式化输出
 * @param buf 输出缓冲区（不可为 NULL）
 * @return 0 成功，-1 失败
 */
int vrf_os_show(GString *buf);

#endif /* VRF_OS_H */
