/**
 * @file   bgp_vrf.h
 * @brief  BGP VRF 层结构定义（聚合 sess_hash 和 inst_hash）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef BGP_VRF_H
#define BGP_VRF_H

#include <glib.h>
#include <stdint.h>

#include "bgp_instance.h"
#include "bgp_session.h"

/* 前向声明：避免在头文件中引入 bgp.h 与 bgp_peer.h 常量名冲突 */
typedef struct bgp_update_result bgp_update_result_t;
typedef struct bgp_rib_update_stats bgp_rib_update_stats_t;

/** 默认公网 VRF ID */
#define BGP_VRF_PUBLIC_ID 0

/** 默认 keepalive 时间（秒） */
#define BGP_TIMER_DEFAULT_KEEPALIVE 60
/** 默认 hold time（秒） */
#define BGP_TIMER_DEFAULT_HOLD 180
/** 默认 connect-retry 时间（秒） */
#define BGP_TIMER_DEFAULT_CONNECT_RETRY 10

/**
 * @brief BGP VRF 结构（持有该 VRF 下所有会话和地址族实例）
 */
typedef struct bgp_vrf
{
    uint32_t vrf_id;        /**< VRF ID，0 为默认公网 VRF */
    uint32_t router_id;     /**< VRF Router ID（主机序 32 位，0 表示未配置） */
    uint16_t keepalive;     /**< keepalive 定时器（秒），默认 60 */
    uint16_t hold_time;     /**< hold time（秒），默认 180，须大于 keepalive */
    uint16_t connect_retry; /**< TCP 主动连接失败后重试间隔（秒），默认 10 */
    GHashTable *sess_hash;  /**< addr_str -> bgp_session_t*（持有所有权） */
    GHashTable *inst_hash;  /**< (afi<<16|safi) -> bgp_instance_t*（持有所有权，g_direct_hash） */
} bgp_vrf_t;

/**
 * @brief 创建 VRF 结构
 * @param vrf_id VRF ID
 * @return 新建的 bgp_vrf_t 指针
 */
bgp_vrf_t *bgp_vrf_create(uint32_t vrf_id);

/**
 * @brief 销毁 VRF 结构（同时释放所有 session 和 instance）
 * @param vrf bgp_vrf_t 指针（允许为 NULL）
 */
void bgp_vrf_destroy(bgp_vrf_t *vrf);

/**
 * @brief 将 session 添加到 VRF 的会话哈希表（所有权转移给 vrf）
 * @param vrf     VRF 结构
 * @param session 会话结构
 */
void bgp_vrf_add_session(bgp_vrf_t *vrf, bgp_session_t *session);

/**
 * @brief 从 VRF 的会话哈希表中删除指定 session（并销毁）
 * @param vrf  VRF 结构
 * @param addr 邻居 IP 地址
 */
void bgp_vrf_del_session(bgp_vrf_t *vrf, const net_addr_t *addr);

/**
 * @brief 在 VRF 的会话哈希表中查找指定 session
 * @param vrf  VRF 结构
 * @param addr 邻居 IP 地址
 * @return 会话指针（借用，不可释放），未找到返回 NULL
 */
bgp_session_t *bgp_vrf_find_session(bgp_vrf_t *vrf, const net_addr_t *addr);

/**
 * @brief 在指定地址族下使能邻居，创建 bgp_peer_t 并挂入 inst
 * @param vrf  VRF 结构
 * @param afi  地址族
 * @param safi 子地址族
 * @param addr 邻居 IP 地址
 * @return 0 成功，-1 失败（session 不存在等）
 */
int bgp_vrf_af_enable_neighbor(bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi, const net_addr_t *addr);

/**
 * @brief 在指定地址族下停用邻居，从 inst 中移除并销毁 bgp_peer_t
 * @param vrf  VRF 结构
 * @param afi  地址族
 * @param safi 子地址族
 * @param addr 邻居 IP 地址
 * @return 0 成功，-1 失败
 */
int bgp_vrf_af_disable_neighbor(bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi, const net_addr_t *addr);

/**
 * @brief 收集邻居在所有地址族实例下的 peer 列表（借用引用，调用方负责 g_list_free，不可销毁元素）
 * @param vrf  VRF 结构
 * @param addr 邻居 IP 地址
 * @return GList* of bgp_peer_t*，无匹配返回 NULL
 */
GList *bgp_vrf_get_session_peers(bgp_vrf_t *vrf, const net_addr_t *addr);

/**
 * @brief 检查邻居是否在任意地址族实例下使能
 * @param vrf  VRF 结构
 * @param addr 邻居 IP 地址
 * @return TRUE 若至少在一个 AF 下使能，否则 FALSE
 */
gboolean bgp_vrf_neighbor_has_any_af(bgp_vrf_t *vrf, const net_addr_t *addr);

/**
 * @brief 在 VRF 中获取或创建指定 AF 的 bgp_instance_t
 * @param vrf  VRF 结构
 * @param afi  地址族
 * @param safi 子地址族
 * @return bgp_instance_t 指针（不可释放，VRF 持有所有权）
 */
bgp_instance_t *bgp_vrf_get_or_create_instance(bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 从 VRF 中删除指定 AF 的 bgp_instance_t（同时销毁其所有 peer）
 * @param vrf  VRF 结构
 * @param afi  地址族
 * @param safi 子地址族
 */
void bgp_vrf_del_instance(bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 清理某邻居在 VRF 下的全部路由
 * @param vrf  目标 VRF
 * @param addr 邻居地址
 * @return 删除的 route 数
 */
uint32_t bgp_vrf_purge_session_routes(bgp_vrf_t *vrf, const net_addr_t *addr);

/**
 * @brief 获取 VRF 所有 AF RIB 统计
 */
uint32_t bgp_vrf_rib_head_count(const bgp_vrf_t *vrf);
uint32_t bgp_vrf_rib_route_count(const bgp_vrf_t *vrf);

/**
 * @brief 重置 VRF 内所有有活跃连接的 session（router-id / timer 变更后调用）
 *
 * 遍历 vrf->sess_hash，对每个持有 pri_conn 或 sec_conn 的 session 调用
 * bgp_neighbor_down，让其发送 NOTIFICATION 并按 connect-retry 调度重连。
 */
void bgp_vrf_reset_all_sessions(bgp_vrf_t *vrf);

/**
 * @brief 按当前 VRF connect-retry 配置，重排已挂起的 retry 定时器
 *
 * 用于 connect-retry 参数动态修改后，让已存在的 retry timer 立即生效新间隔。
 */
void bgp_vrf_rearm_retry_timers(bgp_vrf_t *vrf);

#endif /* BGP_VRF_H */
