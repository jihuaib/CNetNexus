/**
 * @file   bgp_rd.h
 * @brief  BGP Route Distinguisher 处理结构（公网=全 0 RD，私网占非 0 RD）
 * @author jhb
 * @date   2026/05/01
 */
#ifndef BGP_RD_H
#define BGP_RD_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "bgp.h"

typedef struct bgp_instance bgp_instance_t;
typedef struct bgp_protocol bgp_protocol_t;
typedef struct bgp_rib bgp_rib_t;

/**
 * @brief RD entry 复合键：在 protocol 级别全局唯一
 *
 * 公网 entry 的 rd 字段全 0；非 VPN AF 也用 rd=0 的 entry 占位，统一查表入口。
 */
typedef struct bgp_rd_key
{
    bgp_rd_t rd;     /**< 8 字节 RD（大端原始字节） */
    uint32_t vrf_id; /**< 所属 VRF ID */
    uint16_t afi;    /**< 地址族 */
    uint8_t safi;    /**< 子地址族 */
    uint8_t _pad;    /**< 保留 0，使 sizeof=16 对齐稳定 */
} bgp_rd_key_t;

/**
 * @brief RD entry：每个 RD 拥有独立 RIB；后续可挂 import/export RT 等元数据
 */
typedef struct bgp_rd_entry
{
    bgp_rd_key_t key;     /**< 复合键，{rd, vrf, afi, safi} */
    bgp_instance_t *inst; /**< 反向指针（借用，方便回查 vrf/af） */
    bgp_rib_t *rib;       /**< 本 RD 的 RIB（持有所有权） */
    /* 预留：import_rts / export_rts / route_count_cache / per-RD 配置 ... */
} bgp_rd_entry_t;

/**
 * @brief 判断 SAFI 是否属于 VPN 类（NLRI 自带 RD）
 * @param safi 子地址族
 * @return TRUE 表示 VPN 类（VPN_UNICAST / VPN_FLOWSPEC），FALSE 否则
 */
static inline bool bgp_safi_is_vpn(bgp_safi_t safi)
{
    return safi == BGP_SAFI_VPN_UNICAST || safi == BGP_SAFI_VPN_FLOWSPEC;
}

/**
 * @brief 判断 RD 是否为全 0（公网占位）
 */
static inline bool bgp_rd_is_public(const bgp_rd_t *rd)
{
    if (!rd)
    {
        return true;
    }
    for (int i = 0; i < 8; i++)
    {
        if (rd->bytes[i] != 0)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 全 0 RD 常量（公网）
 */
extern const bgp_rd_t BGP_RD_PUBLIC;

/**
 * @brief 复合键 hash（按 16 字节整体 hash）
 */
guint bgp_rd_key_hash(gconstpointer key);

/**
 * @brief 复合键比较（memcmp 16 字节）
 */
gboolean bgp_rd_key_equal(gconstpointer a, gconstpointer b);

/**
 * @brief 8 字节 RD hash（用于 inst->rd_entries，key 只是 RD 不含 afi/safi/vrf）
 */
guint bgp_rd_hash(gconstpointer key);

/**
 * @brief 8 字节 RD 比较
 */
gboolean bgp_rd_equal(gconstpointer a, gconstpointer b);

/**
 * @brief 从 NLRI 中提取 RD（VPN AF）；非 VPN AF 写入全 0
 * @param nlri NLRI 条目
 * @param out  输出 RD（不可为 NULL）
 */
void bgp_nlri_extract_rd(const bgp_nlri_entry_t *nlri, bgp_rd_t *out);

/**
 * @brief 在 protocol->rd_hash 中查找 RD entry
 * @return entry 指针（借用，不可释放），未找到返回 NULL
 */
bgp_rd_entry_t *bgp_protocol_find_rd_entry(bgp_protocol_t *proto, uint16_t afi, uint8_t safi, uint32_t vrf_id,
                                           const bgp_rd_t *rd);

/**
 * @brief 查找或创建 RD entry，并将 entry 挂入 inst->rd_entries
 * @param proto   协议结构
 * @param inst    所属 AF 实例（必填，将被设为 entry->inst 并挂入 inst->rd_entries）
 * @param rd      RD（NULL 表示公网，等价于全 0）
 * @return entry 指针
 */
bgp_rd_entry_t *bgp_protocol_ensure_rd_entry(bgp_protocol_t *proto, bgp_instance_t *inst, const bgp_rd_t *rd);

/**
 * @brief 从 protocol 删除 RD entry（同时从 inst->rd_entries 解挂、销毁 RIB）
 */
void bgp_protocol_remove_rd_entry(bgp_protocol_t *proto, bgp_instance_t *inst, const bgp_rd_t *rd);

/**
 * @brief 将 inst 名下所有 RD entry 从 protocol 中删除（instance 销毁前调用）
 */
void bgp_protocol_remove_all_inst_rd_entries(bgp_protocol_t *proto, bgp_instance_t *inst);

/**
 * @brief 返回 rd_hash value 销毁回调（供 bgp_protocol_create 注册）
 */
GDestroyNotify bgp_rd_entry_destroy_notify(void);

#endif /* BGP_RD_H */
