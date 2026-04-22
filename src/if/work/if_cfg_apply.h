/**
 * @file   if_cfg_apply.h
 * @brief  接口配置内存态应用接口（CLI / DB 恢复共用）
 * @author jhb
 * @date   2026/03/08
 */
#ifndef IF_CFG_APPLY_H
#define IF_CFG_APPLY_H

#include <glib.h>

#include "if_map.h"

/**
 * @brief 按逻辑名查找接口内存条目
 * @param logical_name 逻辑名，如 "GE-1"
 * @return 找到时返回条目指针，否则返回 NULL
 */
if_map_entry_t *if_cfg_find_entry(const char *logical_name);

/**
 * @brief 应用 ip address/ipv6 address 及对应 no 命令到内存和物理接口
 * @param is_no        TRUE=删除 IP，FALSE=配置 IP
 * @param logical_name 逻辑接口名
 * @param prefix       IP/前缀（is_no=TRUE 时忽略）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int if_cfg_apply_ip(gboolean is_no, const char *logical_name, const net_prefix_t *prefix);

/**
 * @brief 应用 shutdown / no shutdown 到内存和物理接口
 * @param is_no      TRUE=no shutdown（UP），FALSE=shutdown（DOWN）
 * @param logical_name 逻辑接口名
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int if_cfg_apply_shutdown(gboolean is_no, const char *logical_name);

/**
 * @brief 确保 loop 接口内存条目和 OS dummy 接口存在（不操作 DB，供 DB 恢复使用）
 * @param loop_id loop 接口编号（1-1024）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int if_cfg_loop_ensure(uint32_t loop_id);

/**
 * @brief 创建 loop 接口：内存条目 + OS dummy 接口 + DB 写入（CLI 命令专用）
 * @param loop_id loop 接口编号（1-1024）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int if_cfg_loop_create(uint32_t loop_id);

/**
 * @brief 删除 loop 接口：清理 IP + OS 接口 + 内存条目 + DB 删除（CLI no 命令专用）
 * @param loop_id loop 接口编号（1-1024）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int if_cfg_loop_delete(uint32_t loop_id);

/**
 * @brief 链路恢复：接口重建后重新 UP、下发 IP 和直连路由（IF work 线程调用）
 *
 * 当 Netlink 监听到 RTM_NEWLINK 且 ifindex 发生变化时，由 IF work 线程调用。
 * 将接口 UP、重新下发已配置的 IPv4/IPv6 地址到内核、注入直连路由到 RIB。
 *
 * @param logical_name 逻辑接口名（如 "GE-1"）
 * @param new_ifindex  新的内核接口索引
 * @param pfx_v4       快照的 IPv4 前缀（可能未配置，family=0）
 * @param pfx_v6       快照的 IPv6 前缀（可能未配置，family=0）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int if_cfg_recover_link(const char *logical_name, uint32_t new_ifindex, const net_prefix_t *pfx_v4,
                        const net_prefix_t *pfx_v6);

/**
 * @brief 链路丢失：接口被销毁后撤销直连路由（IF work 线程调用）
 *
 * 当 Netlink 监听到 RTM_DELLINK 时，由 IF work 线程调用。
 * 使用保存的旧 ifindex 构造路由条目并从 RIB 中撤销直连路由。
 *
 * @param logical_name 逻辑接口名（如 "GE-1"）
 * @param old_ifindex  接口被销毁前的内核接口索引
 * @param pfx_v4       快照的 IPv4 前缀
 * @param pfx_v6       快照的 IPv6 前缀
 */
void if_cfg_handle_link_down(const char *logical_name, uint32_t old_ifindex, const net_prefix_t *pfx_v4,
                             const net_prefix_t *pfx_v6);

#endif /* IF_CFG_APPLY_H */
