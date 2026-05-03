/**
 * @file   fib_os.h
 * @brief  FIB OS 下发接口
 */
#ifndef FIB_OS_H
#define FIB_OS_H

#include <glib.h>
#include <netinet/in.h>

#include "fib.h"

int fib_os_route_install_ip(const fib_route_entry_t *route);
int fib_os_route_install_tunnel(const fib_route_entry_t *route, const fib_tunnel_entry_t *tunnel);
int fib_os_route_withdraw(const fib_route_entry_t *route);
int fib_os_show(GString *buf, sa_family_t family);

#endif /* FIB_OS_H */
