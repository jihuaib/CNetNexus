/**
 * @file   bgp_cfg_apply.h
 * @brief  BGP 配置内存态应用接口（CLI / DB 恢复共用）
 * @author jhb
 * @date   2026/03/07
 */
#ifndef BGP_CFG_APPLY_H
#define BGP_CFG_APPLY_H

#include <glib.h>
#include <stdint.h>

#include "bgp_session.h"
#include "bgp_vrf.h"

/**
 * @brief 应用 bgp/no bgp 到内存状态（不做 DB 持久化）
 * @param as_number  bgp <as-number> 参数（is_no=TRUE 时忽略）
 * @param is_no      TRUE=no bgp，FALSE=bgp <as-number>
 * @return 应用结果
 */
uint32_t bgp_cfg_apply_protocol(gboolean is_no, uint32_t as_number);

/**
 * @brief 应用 af/no af 配置到内存
 */
uint32_t bgp_cfg_apply_instance(gboolean is_no, bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 应用 neighbor/no neighbor（BGP 视图）到内存
 */
uint32_t bgp_cfg_apply_neighbor(gboolean is_no, bgp_vrf_t *vrf, const net_addr_t *addr, uint32_t remote_as);

/**
 * @brief 应用 AF 视图 neighbor enable/no neighbor 到内存
 */
uint32_t bgp_cfg_apply_af_neighbor(gboolean is_no, bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi,
                                   const net_addr_t *addr);

/**
 * @brief 应用 router-id/no router-id 到内存
 */
uint32_t bgp_cfg_apply_router_id(gboolean is_no, bgp_vrf_t *vrf, const char *router_id);

/**
 * @brief 应用 timers/no timers 到内存
 */
uint32_t bgp_cfg_apply_timers(gboolean is_no, bgp_vrf_t *vrf, uint16_t keepalive, uint16_t hold_time);

/**
 * @brief 应用 connect-retry/no connect-retry 到内存
 */
uint32_t bgp_cfg_apply_connect_retry(gboolean is_no, bgp_vrf_t *vrf, uint16_t connect_retry);

/**
 * @brief 应用 open-capability/no open-capability 到内存
 */
uint32_t bgp_cfg_apply_open_capability(gboolean is_no, bgp_session_t *sess, uint32_t cap_bit);

#endif /* BGP_CFG_APPLY_H */
