/**
 * @file   bgp_vrf_export.h
 * @brief  VRF 路由导出到同 AF VPN：把私网 IPv4/IPv6 unicast 最优路由按各自 RD
 *         导入 public VPNv4/VPNv6 RIB，并复用该流水线支持 IPv4 EVPN 导出。
 * @author jhb
 * @date   2026/05/31
 *
 * 设计要点：
 *  - 状态挂在各 public VPN/EVPN instance 上(inst->vrf_export_state)。
 *  - 源：每个私网 VRF 的同 AF unicast RIB 的 VALID best。目标：对应 public VPN instance，
 *    按源 VRF 的 RD 落到 rd_entry。
 *  - 导出时合入该 VRF 的 export RT、写 RD；共享 VPN Loc-RIB 保持 Prefix-SID 中性。
 *    最终由 update-group 按 `neighbor <peer> srv6-sid` 选择 MPLS 或 End.DT SID 编码。
 *  - 分批：enable 全量扫描只入 pending 队列，由 worker BGP_WORKER_EVENT_VRF_EXPORT
 *    事件每批 256 条处理 + eventfd 自重排，不阻塞 worker。
 *  - 增量：私网 VRF unicast calc 完成后调 bgp_vrf_export_on_calc_done() 补推。
 *
 * 注意：本子系统负责把路由灌进 VPN RIB；实际邻居发布由 VPN UPDATE 编码器完成。
 */
#ifndef BGP_VRF_EXPORT_H
#define BGP_VRF_EXPORT_H

#include <glib.h>
#include <stdint.h>

#include "bgp_instance.h"
#include "bgp_rib.h"

/** 每批处理的源 head 数(worker 分批 + 自重排的批大小) */
#define BGP_VRF_EXPORT_BATCH 256

/**
 * @brief VRF 导出状态(挂在 public VPN/EVPN instance 上)
 */
typedef struct bgp_vrf_export_state
{
    GQueue *pending;            /**< 待处理的源 unicast rthead(入队 bgp_rib_head_ref，出队 unref) */
    uint32_t pending_count;     /**< pending 队列长度 */
    gboolean enabled;           /**< AF export 已使能；disable/quiesce 后拒绝新的跨实例任务 */
    uint32_t failed_count;      /**< 本轮因 service SID 构造/分配失败的 head 数 */
    gint64 retry_due_usec;      /**< SID 失败后下次允许消费 pending 的 monotonic 时刻；0=立即 */
    uint8_t retry_backoff_exp;  /**< 连续 SID 失败的退避指数（有上限） */
    gboolean retry_event_armed; /**< 已为到期重试投递过 worker 事件 */
} bgp_vrf_export_state_t;

/*
 * 指针生命周期约定（重要）：
 *  - 唯一长期持有的跨节点指针是导出节点的 export->src_route，指向其来源(私网 unicast best)节点，
 *    并对来源节点 bgp_route_node_borrow_ref 钉住，防止来源被 unreach/RIB 销毁时提前释放
 *    (来源被借用时只被标 STALE 留在 RIB 链表上，待 borrow_unref 归零后真正释放)。
 *  - 不存"来源->导出"反向表：给定来源节点的 head + 该 VRF 的 RD 即可现推 vpn_nlri，
 *    在 vpnv4 RIB 中查到导出节点，无需额外裸指针，避免导出节点被回收后反表悬空。
 *  - 导出节点本身(vpnv4 RIB 内、合成来源、BGP_ROUTE_FLAG_IMPORT)仅由本子系统管理；
 *    其释放只走本子系统的 detach_src + unreach 路径，detach_src 负责释放对来源的 borrow。
 *  - 每条拆除路径都释放 borrow：process_one(best 切换/消失)、disable、inst_destroy、purge_source_inst。
 */

/**
 * @brief 为 public VPN/EVPN instance 初始化导出状态
 *
 * 由 bgp_instance_create() 在 inst 满足 public VPN/EVPN 条件时调用。
 * 其它 instance 调用为 no-op。
 */
void bgp_vrf_export_inst_init(bgp_instance_t *inst);

/**
 * @brief 销毁 public VPN/EVPN instance 的导出状态(抽干 pending 队列)
 */
void bgp_vrf_export_inst_destroy(bgp_instance_t *inst);

/**
 * @brief 取当前 public vpnv4 instance(导出目标)；未使能返回 NULL
 *
 * 同时作为"vpnv4 是否已使能"的判据。
 */
bgp_instance_t *bgp_vrf_export_target_inst(void);

/**
 * @brief 取指定 public VPN 类 instance(当前支持 VPNv4/VPNv6/EVPN)；未使能返回 NULL
 */
bgp_instance_t *bgp_vrf_export_target_inst_by_af(bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 使能指定 VPN/EVPN 导出：全量扫描同 AF 私网 unicast RIB 入 pending 并投事件
 * @param vpn_inst public VPN/EVPN instance
 * @return 0 成功，-1 参数非法
 */
int bgp_vrf_export_enable(bgp_instance_t *vpn_inst);

/**
 * @brief 去使能指定 VPN/EVPN 导出：撤销所有已导出的路由，清空 pending
 * @param vpn_inst public VPN/EVPN instance
 * @return 0 成功，-1 参数非法
 */
int bgp_vrf_export_disable(bgp_instance_t *vpn_inst);

/**
 * @brief 私网 VRF 的 unicast calc 完成后调用：若同 AF VPN 已使能，补推该 head 到 pending
 *
 * 由 bgp_calc_route_select() 末尾调用(与 bgp_import_rib_on_calc_done 并列)。
 * @param src_inst 源 instance(应为私网 VRF 的 IPv4/IPv6 unicast)
 * @param head     发生 best 变化的 rthead
 */
void bgp_vrf_export_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head);

/**
 * @brief 某私网 VRF 配置 RD 后调用：把该 VRF 已有 unicast 路由补灌到已启用的 VPN AF
 *
 * 用于"VPN AF 先使能、私网 VRF 后配 RD"的时序：enable 全量扫描时该 VRF 因无 RD 被跳过，
 * RD 配上来后由本函数把已有路由扫入 pending 重新导出。VPN AF 未使能时为 no-op。
 * @param vrf_id 私网 VRF ID
 */
void bgp_vrf_export_backfill_vrf(uint32_t vrf_id);

/**
 * @brief 发送时解析某导出 best 路由应携带的 VPN 标签（per-vrf 聚合，向 TUNNEL 申请并缓存）
 *
 * loc-rib 不带标签，update_group 通告 vpnv4 本地导出路由时调用本函数取标签注入 NLRI。
 * @param best vpnv4 RIB 内的本地导出 best 路由（须为 IMPORT 且 src_route 有效）
 * @return 标签值（>0）；0 表示暂不可得，调用方应 hold（不发送），待下次 pub 重试
 */
uint32_t bgp_vrf_export_resolve_send_label(const bgp_route_node_t *best);

/**
 * @brief 释放某私网 VRF 的 per-vrf VPN 标签（VRF 销毁/不再导出时调用）
 * @param vrf 源私网 VRF（vpn_label==0 时为 no-op）
 */
void bgp_vrf_export_release_vrf_label(bgp_vrf_t *vrf);

/** Release cached End.DT4/End.DT6 SIDs owned by a source VRF. */
void bgp_vrf_export_release_srv6_sids(bgp_vrf_t *vrf);

/** Release one address-family's cached End.DT service SID. */
int bgp_vrf_export_release_srv6_sid(bgp_vrf_t *vrf, bgp_afi_t afi);

/** Release all BGP-owned service SIDs in this VRF/AF owner scope, regardless
 * of locator, and clear the local cache.  Used for rollback/cold reconcile. */
int bgp_vrf_export_reconcile_srv6_sid_absent(bgp_vrf_t *vrf, bgp_afi_t afi);

/** Allocate/program the End.DT SID selected by one private unicast AF's locator. */
int bgp_vrf_export_prepare_srv6_sid(bgp_instance_t *src_inst);

/** Attach a cached End.DT SID to one local VPN export for a SID update-group.
 * Returns success with no Prefix-SID when the source AF has no locator, so the
 * normal MPLS VPN label remains the per-neighbor fallback. */
int bgp_vrf_export_apply_srv6_sid(const bgp_route_node_t *best, bgp_attr_t *out_attr);

/** Rebuild one private unicast instance's VPN exports after source configuration changes. */
int bgp_vrf_export_reprocess_instance(bgp_instance_t *src_inst);

/** Withdraw only one private unicast instance's same-AF public VPN exports. */
void bgp_vrf_export_purge_source_vpn_inst(bgp_instance_t *src_inst);

/**
 * @brief 私网 VRF 的 unicast instance 销毁前调用：撤销其名下所有同 AF 已导出 VPN 路由
 *
 * 必须在源 instance 的 RIB 释放前调用(此时源 route 节点仍存活)，否则导出节点持有的
 * borrow 引用会让源节点滞留为 STALE 孤儿且 VPN 路由残留。
 * 由 bgp_instance_destroy() 在私网 IPv4/IPv6 unicast 实例上调用。
 * @param src_inst 即将销毁的源 instance
 */
void bgp_vrf_export_purge_source_inst(bgp_instance_t *src_inst);

/**
 * @brief 批量处理 VPN/EVPN inst 的 pending 队列(每次最多 batch 条)
 *
 * 由 worker BGP_WORKER_EVENT_VRF_EXPORT 事件回调驱动。
 * @return 实际处理条目数
 */
int bgp_vrf_export_queue_process(bgp_instance_t *vpn_inst, int batch);

/** pending 中存在当前可消费的条目（未处于 SID 退避窗口）。 */
gboolean bgp_vrf_export_queue_ready(const bgp_instance_t *vpn_inst);

/** 只读返回 public VPN/EVPN 实例尚未处理的导出队列长度（含退避项）。 */
uint32_t bgp_vrf_export_pending_count(const bgp_instance_t *vpn_inst);

/** worker 周期 tick：将已到期的 SID 退避队列重投工作事件。 */
void bgp_vrf_export_retry_tick(void);

/**
 * @brief 同步抽干 vpnv4 inst 的 pending 队列(drain_pending / 关闭路径)
 * @return 实际处理条目数
 */
int bgp_vrf_export_process_pending(bgp_instance_t *vpn_inst);

/**
 * @brief 同步抽干所有 VRF export 目标 instance 的 pending 队列
 */
int bgp_vrf_export_process_all_pending(void);

#endif /* BGP_VRF_EXPORT_H */
