/**
 * @file   isis.h
 * @brief  ISIS 模块公共常量定义
 * @author jhb
 * @date   2026/04/11
 */
#ifndef ISIS_H
#define ISIS_H

#include <stdint.h>

/* ============================================================================
 * ISIS 基本常量
 * ========================================================================== */

/** ISIS Level-1 */
#define ISIS_IS_TYPE_LEVEL_1 1u
/** ISIS Level-2 */
#define ISIS_IS_TYPE_LEVEL_2 2u
/** ISIS Level-1-2 */
#define ISIS_IS_TYPE_LEVEL_1_2 3u

/** ISIS AF: IPv4 */
#define ISIS_AFI_IPV4 1u
/** ISIS AF: IPv6 */
#define ISIS_AFI_IPV6 2u

/** ISIS 默认接口 metric（RFC 5305 wide metric） */
#define ISIS_DEFAULT_IF_METRIC 10u
/** ISIS 接口 metric 上限 */
#define ISIS_MAX_IF_METRIC 16777215u
/** ISIS 默认 hello interval（秒） */
#define ISIS_DEFAULT_HELLO_INTERVAL 10u
/** ISIS hello interval 上限 */
#define ISIS_MAX_HELLO_INTERVAL 65535u
/** ISIS 默认 hold multiplier */
#define ISIS_DEFAULT_HOLD_MULTIPLIER 3u
/** ISIS hold multiplier 上限 */
#define ISIS_MAX_HOLD_MULTIPLIER 100u
/** ISIS 接口默认非 passive */
#define ISIS_DEFAULT_IF_PASSIVE 0u

/** ISIS cost-style: narrow (RFC 1195, TLV 2/128, 6-bit metric) */
#define ISIS_COST_STYLE_NARROW 1u
/** ISIS cost-style: wide (RFC 5305/5308, TLV 22/135/236, 24/32-bit metric) */
#define ISIS_COST_STYLE_WIDE 2u
/** ISIS cost-style 默认值 */
#define ISIS_DEFAULT_COST_STYLE ISIS_COST_STYLE_NARROW
/** narrow metric 上限（6 bits） */
#define ISIS_NARROW_MAX_METRIC 63u

#endif /* ISIS_H */
