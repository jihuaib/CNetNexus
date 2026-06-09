/**
 * @file   bgp_instance.h
 * @brief  BGP 地址族实例结构定义（按 afi/safi 索引，持有 VRF 反向指针）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef BGP_INSTANCE_H
#define BGP_INSTANCE_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bgp.h"
#include "bgp_peer.h"
#include "net_addr.h"

/* bgp_peer.h 已前向声明 bgp_vrf_t，此处直接使用 */
typedef struct bgp_rib bgp_rib_t;
typedef struct bgp_rd_entry bgp_rd_entry_t;
typedef struct bgp_calc_queue bgp_calc_queue_t;
typedef struct bgp_route_flush_queue bgp_route_flush_queue_t;

/**
 * @brief BGP 地址族实例（持有该 AF 下所有已使能邻居的 bgp_peer_t 所有权）
 *
 * RIB 存储结构：每个 RD 一张 RIB，挂在 protocol->rd_hash 的 entry 上。
 * 本实例只通过 rd_entries 持有借用引用列表，便于按 AF 维度遍历所有 RD。
 * 公网（包含非 VPN AF）由 RD 全 0 的 entry 占位，bgp_instance_create 时自动注入。
 */
typedef struct bgp_instance
{
    bgp_afi_t afi;                /**< 地址族 */
    bgp_safi_t safi;              /**< 子地址族 */
    GHashTable *peer_hash;        /**< net_addr_t* -> bgp_peer_t*（持有所有权，按二进制地址索引） */
    GHashTable *rd_entries;       /**< bgp_rd_t* -> bgp_rd_entry_t*（借用，所有权在 protocol->rd_hash） */
    bgp_vrf_t *vrf;               /**< 所属 VRF（借用引用，不持有所有权） */
    uint32_t import_protos;       /**< 已导入协议位掩码：bit N 置 1 表示 protocol=N 已导入 */
    bgp_calc_queue_t *calc_queue; /**< best-path 待处理队列（持有所有权） */
    bgp_route_flush_queue_t *route_flush_queue; /**< ROUTE 下刷待处理队列（持有所有权） */
    GHashTable *attr_table;     /**< bgp_attr_ref_t* -> bgp_attr_ref_t*（本 instance 内属性去重表） */
    GHashTable *attr_id_table;  /**< attr_id -> bgp_attr_ref_t*（本 instance 内反查表） */
    GHashTable *nexthop_by_key; /**< route_nhobj_key_t* -> BGP nexthop entry（本 instance 内） */
    GHashTable *nexthop_by_id;  /**< nexthop_id -> BGP nexthop entry（本 instance 内借用） */
    GList *update_groups;       /**< bgp_update_group_t*（持有所有权），按出向策略分组的发布单元 */
    GList *qp_routes;           /**< bgp_qp_route_cfg_t*（持有所有权），已配置的 QP 自产生路由条目 */
    bool route_select_enabled; /**< 是否对该地址族启用路由优选/发布（默认 false，仅 QP 地址族使用） */
    uint32_t flags;            /**< 实例级策略位（见 BGP_INST_FLAG_*） */
    uint32_t cluster_id;         /**< 本 AF 反射器 Cluster-ID（主机序，0=用 router-id） */
    uint32_t next_attr_id;       /**< 本 instance 下一个可分配 attr_id（从 1 开始） */
    uint32_t import_rib_sources; /**< import-rib 源位掩码（bgp_import_src_t），DB 持久化 */
    void *import_rib_state;      /**< bgp_import_rib 模块内部状态（pending queue / mirror 反向索引等） */
    void *vrf_export_state;      /**< bgp_vrf_export 状态（仅 public vpnv4 instance 非空） */
} bgp_instance_t;

/**
 * @brief 获取实例有效的 Cluster-ID：inst->cluster_id 非 0 用它，否则 fallback 到 vrf->router_id
 */
uint32_t bgp_inst_effective_cluster_id(const bgp_instance_t *inst);

/** 实例策略位：所有出向邻居保留原下一跳（不影响 update-group 划分） */
#define BGP_INST_FLAG_NH_UNCHANGED (1U << 0)
/**
 * 实例策略位：VPN 类地址族(vpnv4/vpnv6/evpn)入向按 import route-target 过滤。
 *
 * 仅 VPN 类 SAFI 实例使用，创建时默认置位(`policy vpn-target`)。置位时收到的 VPN 路由
 * 必须 IRT 命中才接受进本实例 RIB，否则入向丢弃；`no policy vpn-target` 清除该位后
 * VPN 路由一律接受(供 RR 透传)，但导入私网 VRF 仍在 reconcile 阶段按 IRT 命中决定。
 */
#define BGP_INST_FLAG_VPN_TARGET_FILTER (1U << 1)

/**
 * @brief QP 自产生路由配置条目（每个对应一组 [start_dqpn, start_dqpn+count) 的 NLRI）
 */
typedef struct bgp_qp_route_cfg
{
    uint32_t start_dqpn; /**< 起始 DQPN */
    uint32_t count;      /**< NLRI 条数 */
    net_addr_t ip;       /**< 前缀基地址（family 决定 afi） */
    uint8_t mask_len;    /**< 前缀长度 */
    net_addr_t bid;      /**< BID（IPv6 下一跳） */
} bgp_qp_route_cfg_t;

/**
 * @brief 计算 inst_hash 的键值（将 afi/safi 打包为 gpointer，无需堆分配）
 * @param afi  地址族
 * @param safi 子地址族
 * @return gpointer 键值，直接传入 g_hash_table_lookup / insert / remove
 */
static inline gpointer bgp_inst_hash_key(bgp_afi_t afi, bgp_safi_t safi)
{
    return GUINT_TO_POINTER(((guint32)afi << 16) | (guint32)safi);
}

/**
 * @brief 创建地址族实例结构
 * @param afi  地址族
 * @param safi 子地址族
 * @param vrf  所属 VRF（借用引用）
 * @return 新建的 bgp_instance_t 指针
 */
bgp_instance_t *bgp_instance_create(bgp_afi_t afi, bgp_safi_t safi, bgp_vrf_t *vrf);

/**
 * @brief 销毁地址族实例结构（同时销毁所有 peer_hash 中的 bgp_peer_t）
 * @param inst bgp_instance_t 指针（允许为 NULL）
 */
void bgp_instance_destroy(bgp_instance_t *inst);

/**
 * @brief 在当前线程内同步抽干实例的所有数据队列
 *
 * 循环调用 bgp_calc_process_pending / bgp_route_flush_process_pending /
 * bgp_update_group_process_pending，直到所有队列都处理完毕。
 * 用于配置删除/协议关闭路径，确保销毁前完成已排队的数据队列处理。
 */
void bgp_instance_drain_pending(bgp_instance_t *inst);

/**
 * @brief 取该实例下公网（rd=0）entry 的 RIB
 *
 * 等价于 bgp_inst_rib_for_nlri(inst, NLRI_with_zero_rd)，专供没有 NLRI 上下文
 * 但只关心非 VPN AF 的调用方（例如统计/show-all/qp 重哈希）。
 * VPN AF 调用此函数仅返回 RD=0 entry 的 RIB，不代表全部 VPN 路由。
 */
bgp_rib_t *bgp_inst_public_rib(bgp_instance_t *inst);

/**
 * @brief 按 NLRI 选取所属 RIB
 *
 * 非 VPN AF：永远返回公网 entry 的 RIB。
 * VPN AF：从 NLRI 提取 RD，按 RD 在 inst->rd_entries 中查找对应 entry 的 RIB；
 *         未找到返回 NULL（调用方自行决定是否 ensure）。
 */
bgp_rib_t *bgp_inst_rib_for_nlri(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 按 NLRI 选取或创建 RIB（VPN AF 收报文时按 RD 自动 ensure entry）
 */
bgp_rib_t *bgp_inst_rib_ensure_for_nlri(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 遍历回调类型：处理一个 RD entry 的 RIB
 */
typedef void (*bgp_inst_rib_iter_cb)(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer user_data);

/**
 * @brief 遍历实例下所有 RD entry，对每个 entry 的 RIB 调用回调
 *
 * 用于全 AF 维度的统计/show 操作。
 */
void bgp_inst_foreach_rib(bgp_instance_t *inst, bgp_inst_rib_iter_cb cb, gpointer user_data);

#endif /* BGP_INSTANCE_H */
