/**
 * @file   fib_os.h
 * @brief  FIB OS 下发接口
 */
#ifndef FIB_OS_H
#define FIB_OS_H

#include <glib.h>
#include <netinet/in.h>

#include "fib.h"

#ifndef AF_MPLS
#    define AF_MPLS 28
#endif

int fib_os_route_install_ip(const fib_route_entry_t *route);
int fib_os_route_install_tunnel(const fib_route_entry_t *route, const fib_tunnel_entry_t *tunnel);
int fib_os_route_install_srv6(const fib_route_entry_t *route);
int fib_os_route_withdraw(const fib_route_entry_t *route);
int fib_os_mpls_configure(uint32_t platform_labels, gboolean mpls_input);
int fib_os_srv6_configure(void);
int fib_os_ilm_install(const fib_ilm_entry_t *ilm);
int fib_os_ilm_withdraw(const fib_ilm_entry_t *ilm);
int fib_os_srv6_localsid_install(const fib_srv6_localsid_entry_t *entry);
int fib_os_srv6_localsid_withdraw(const fib_srv6_localsid_entry_t *entry);
int fib_os_show(GString *buf, sa_family_t family, uint32_t table_filter, gboolean has_table_filter,
                gboolean include_local_table, uint32_t local_master_ifindex);

#endif /* FIB_OS_H */
