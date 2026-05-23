/**
 * @file   bgp_ext_community.h
 * @brief  BGP Extended Community raw buffer helpers
 */
#ifndef BGP_EXT_COMMUNITY_H
#define BGP_EXT_COMMUNITY_H

#include <stddef.h>
#include <stdint.h>

#include "bgp.h"

/**
 * @brief 将 VRF export RT 按 EXT_COMMUNITY 原始 8 字节条目合入属性
 */
void bgp_ext_community_merge_vrf_export_rts(bgp_attr_t *attr, uint32_t vrf_id, uint16_t afi);

/**
 * @brief 将 EXT_COMMUNITY 原始 buffer 格式化为 show 字符串
 */
void bgp_ext_community_format(const uint8_t *data, uint16_t len, char *buf, size_t bufsz);

#endif /* BGP_EXT_COMMUNITY_H */
