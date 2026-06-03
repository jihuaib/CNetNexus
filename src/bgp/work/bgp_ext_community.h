/**
 * @file   bgp_ext_community.h
 * @brief  BGP Extended Community raw buffer helpers
 */
#ifndef BGP_EXT_COMMUNITY_H
#define BGP_EXT_COMMUNITY_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

#include "bgp.h"
#include "vrf.h"

/**
 * @brief 将 VRF 配置的 RT(原始 8 字节)规范化为线上 Route Target 扩展团体 8 字节
 * @param rt  VRF 配置 RT
 * @param ext 输出 8 字节规范 RT
 * @return TRUE=识别成功；FALSE=无法识别(ext 未定义)
 */
gboolean bgp_ext_community_rt_canon(const vrf_rt_t *rt, uint8_t ext[8]);

/**
 * @brief 判断一条 8 字节扩展团体是否为 Route Target(子类型 0x02)
 */
gboolean bgp_ext_community_is_rt(const uint8_t entry[8]);

/**
 * @brief 将 VRF export RT 按 EXT_COMMUNITY 原始 8 字节条目合入属性
 */
void bgp_ext_community_merge_vrf_export_rts(bgp_attr_t *attr, uint32_t vrf_id, uint16_t afi);

/**
 * @brief 将 EXT_COMMUNITY 原始 buffer 格式化为 show 字符串
 */
void bgp_ext_community_format(const uint8_t *data, uint16_t len, char *buf, size_t bufsz);

#endif /* BGP_EXT_COMMUNITY_H */
