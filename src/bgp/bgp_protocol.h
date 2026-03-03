/**
 * @file   bgp_protocol.h
 * @brief  BGP 全局协议结构定义（含 VRF 哈希表）
 * @author jhb
 * @date   2026/03/02
 */
#ifndef BGP_PROTOCOL_H
#define BGP_PROTOCOL_H

#include <glib.h>
#include <stdint.h>

#include "bgp_vrf.h"

/**
 * @brief BGP 全局协议结构
 *
 * vrf_hash：key = uint32_t*(vrf_id)，value = bgp_vrf_t*（持有所有权）
 * bgp_protocol_create 时自动创建 vrf_id=0 的默认公网 VRF。
 */
typedef struct bgp_protocol
{
    uint32_t as_number;   /**< 本地 AS 号 */
    char router_id[16];   /**< Router ID（点分十进制，默认 "0.0.0.0"） */
    GHashTable *vrf_hash; /**< uint32_t* vrf_id -> bgp_vrf_t*（持有所有权） */
} bgp_protocol_t;

/**
 * @brief 创建 BGP 协议结构，并自动创建 vrf_id=0 的默认公网 VRF
 * @param as_number 本地 AS 号
 * @return 新建的协议结构指针
 */
bgp_protocol_t *bgp_protocol_create(uint32_t as_number);

/**
 * @brief 销毁 BGP 协议结构（同时释放全部 VRF、session、instance）
 * @param proto 协议结构指针（允许为 NULL）
 */
void bgp_protocol_destroy(bgp_protocol_t *proto);

/**
 * @brief 根据 vrf_id 查找 VRF
 * @param proto  协议结构
 * @param vrf_id VRF ID
 * @return bgp_vrf_t 指针（借用，不可释放），未找到返回 NULL
 */
bgp_vrf_t *bgp_protocol_get_vrf(bgp_protocol_t *proto, uint32_t vrf_id);

/**
 * @brief 查找 VRF，不存在时创建
 * @param proto  协议结构
 * @param vrf_id VRF ID
 * @param name   VRF 名称（创建时使用）
 * @return bgp_vrf_t 指针
 */
bgp_vrf_t *bgp_protocol_get_or_create_vrf(bgp_protocol_t *proto, uint32_t vrf_id, const char *name);

#endif /* BGP_PROTOCOL_H */
