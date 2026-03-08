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
 * @brief 应用 ip address / no ip address 到内存和物理接口
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

#endif /* IF_CFG_APPLY_H */
