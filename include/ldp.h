/**
 * @file   ldp.h
 * @brief  LDP 模块公共常量定义
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_H
#define LDP_H

#include <stdint.h>

/* ============================================================================
 * LDP 协议基础常量（RFC 5036）
 * ========================================================================== */

/** LDP 标准 TCP/UDP 端口 */
#define LDP_PORT 646u

/** LDP 默认 hello 间隔（毫秒） */
#define LDP_DEFAULT_HELLO_INTERVAL_MS 5000u
/** LDP 默认 hold 时间（毫秒） */
#define LDP_DEFAULT_HOLD_TIME_MS 15000u
/** LDP 默认 keepalive 间隔（毫秒） */
#define LDP_DEFAULT_KEEPALIVE_INTERVAL_MS 10000u

/** LDP hello/hold 时间下限（毫秒） */
#define LDP_MIN_HELLO_INTERVAL_MS 100u
/** LDP hello/hold 时间上限（毫秒） */
#define LDP_MAX_HELLO_INTERVAL_MS 65535000u

/** LDP keepalive 上限（毫秒） */
#define LDP_MAX_KEEPALIVE_INTERVAL_MS 65535000u

/** LDP 接口默认未使能 */
#define LDP_DEFAULT_IF_ENABLED 0u

#endif /* LDP_H */
