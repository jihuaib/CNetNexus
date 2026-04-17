/**
 * @file   bgp_instance.h
 * @brief  BGP 地址族实例结构定义（按 afi/safi 索引，持有 VRF 反向指针）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef BGP_INSTANCE_H
#define BGP_INSTANCE_H

#include <glib.h>
#include <stddef.h>

#include "bgp.h"
#include "bgp_peer.h"

/* bgp_peer.h 已前向声明 bgp_vrf_t，此处直接使用 */
typedef struct bgp_rib bgp_rib_t;
typedef struct bgp_calc_queue bgp_calc_queue_t;
typedef struct bgp_route_flush_queue bgp_route_flush_queue_t;

/**
 * @brief BGP 地址族实例（持有该 AF 下所有已使能邻居的 bgp_peer_t 所有权）
 */
typedef struct bgp_instance
{
    bgp_afi_t afi;         /**< 地址族 */
    bgp_safi_t safi;       /**< 子地址族 */
    GHashTable *peer_hash; /**< net_addr_t* -> bgp_peer_t*（持有所有权，按二进制地址索引） */
    bgp_rib_t *rib; /**< 该 AFI/SAFI 的内存 RIB（持有所有权，最优路径为每个 rthead 链表首元素） */
    bgp_vrf_t *vrf;               /**< 所属 VRF（借用引用，不持有所有权） */
    uint32_t import_protos;       /**< 已导入协议位掩码：bit N 置 1 表示 protocol=N 已导入 */
    bgp_calc_queue_t *calc_queue; /**< best-path 待处理队列（持有所有权） */
    bgp_route_flush_queue_t *route_flush_queue; /**< ROUTE 下刷待处理队列（持有所有权） */
    GList *update_groups; /**< bgp_update_group_t*（持有所有权），按出向策略分组的发布单元 */
} bgp_instance_t;

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

#endif /* BGP_INSTANCE_H */
