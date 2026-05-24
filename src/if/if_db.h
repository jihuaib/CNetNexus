/**
 * @file   if_db.h
 * @brief  接口模块 DB 操作接口
 * @author jhb
 * @date   2026/04/21
 */
#ifndef IF_DB_H
#define IF_DB_H

#include <stdint.h>

#include "net_addr.h"

int if_db_init(void);
int if_db_restore(void);

/**
 * @brief VRF 重启 re-sync：只重恢复 vrf_name 非空且非 public 的接口行。
 */
int if_db_restore_vrf_bound(void);

int if_db_ensure_record(const char *ifname);
int if_db_del_record(const char *ifname);
int if_db_update_ip(const char *ifname, int is_ipv6, const char *ip_str, uint8_t prefix_len);
int if_db_update_shutdown(const char *ifname, int shutdown);
int if_db_update_vrf(const char *ifname, const char *vrf_name);

#endif /* IF_DB_H */
