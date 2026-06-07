/**
 * @file   lldp.h
 * @brief  LLDP 模块公共常量定义
 * @author jhb
 * @date   2026/06/07
 */
#ifndef LLDP_H
#define LLDP_H

#include <stdint.h>

#define LLDP_DEFAULT_TX_INTERVAL_SEC 30u
#define LLDP_DEFAULT_HOLD_MULTIPLIER 4u
#define LLDP_DEFAULT_REINIT_DELAY_SEC 2u
#define LLDP_DEFAULT_TX_DELAY_SEC 2u

#define LLDP_MIN_TX_INTERVAL_SEC 5u
#define LLDP_MAX_TX_INTERVAL_SEC 32768u
#define LLDP_MIN_HOLD_MULTIPLIER 2u
#define LLDP_MAX_HOLD_MULTIPLIER 10u

#define LLDP_IF_ADMIN_DISABLED 1u
#define LLDP_IF_ADMIN_RX_ONLY 2u
#define LLDP_IF_ADMIN_TX_RX 3u
#define LLDP_IF_ADMIN_TX_ONLY 4u

#define LLDP_PORT_DESC_MAX 256u

#endif /* LLDP_H */
