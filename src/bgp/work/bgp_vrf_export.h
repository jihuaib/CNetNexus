/**
 * @file   bgp_vrf_export.h
 * @brief  VRF 路由导出到 vpnv4：vpnv4 地址族(public VRF, afi=ipv4 safi=vpn-unicast)
 *         使能时，把所有私网 VRF 的 ipv4-unicast 最优路由按各自 RD 导入 vpnv4 RIB。
 * @author jhb
 * @date   2026/05/31
 *
 * 设计要点：
 *  - 状态挂在 public vpnv4 instance 上(inst->vrf_export_state)，仅该 inst 非空。
 *  - 源：每个私网 VRF 的 (ipv4, unicast) instance RIB 的 VALID best(含 peer 学习与
 *    import-route 本地引入)。目标：public vpnv4 instance，按源 VRF 的 RD 落到对应 rd_entry。
 *  - 导出时合入该 VRF 的 export RT、写 RD、写 per-VRF 单标签(VPN label)。
 *  - 分批：enable 全量扫描只入 pending 队列，由 worker BGP_WORKER_EVENT_VRF_EXPORT
 *    事件每批 256 条处理 + eventfd 自重排，不阻塞 worker。
 *  - 增量：私网 VRF unicast calc 完成后调 bgp_vrf_export_on_calc_done() 补推。
 *
 * 注意：本子系统只负责把路由灌进 vpnv4 RIB；向 vpnv4 邻居发布依赖 vpnv4 报文编码器
 *       (afi=ipv4 safi=vpn-unicast 的 bgp_pkt_af_enc，尚未实现，属后续工作)。
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
 * @brief VRF 导出状态(挂在 public vpnv4 instance 上)
 */
typedef struct bgp_vrf_export_state
{
    GQueue *pending;        /**< 待处理的源 unicast rthead(入队 bgp_rib_head_ref，出队 unref) */
    uint32_t pending_count; /**< pending 队列长度 */
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
 * @brief 为 public vpnv4 instance 初始化导出状态
 *
 * 由 bgp_instance_create() 在 inst 满足 (public VRF, ipv4, vpn-unicast) 时调用。
 * 其它 instance 调用为 no-op。
 */
void bgp_vrf_export_inst_init(bgp_instance_t *inst);

/**
 * @brief 销毁 public vpnv4 instance 的导出状态(抽干 pending 队列)
 */
void bgp_vrf_export_inst_destroy(bgp_instance_t *inst);

/**
 * @brief 取当前 public vpnv4 instance(导出目标)；未使能返回 NULL
 *
 * 同时作为"vpnv4 是否已使能"的判据。
 */
bgp_instance_t *bgp_vrf_export_target_inst(void);

/**
 * @brief 使能 vpnv4 导出：全量扫描所有私网 VRF 的 unicast RIB 入 pending 并投事件
 * @param vpn_inst public vpnv4 instance
 * @return 0 成功，-1 参数非法
 */
int bgp_vrf_export_enable(bgp_instance_t *vpn_inst);

/**
 * @brief 去使能 vpnv4 导出：撤销所有已导出的 VPN 路由，清空 pending
 * @param vpn_inst public vpnv4 instance
 * @return 0 成功，-1 参数非法
 */
int bgp_vrf_export_disable(bgp_instance_t *vpn_inst);

/**
 * @brief 私网 VRF 的 ipv4-unicast calc 完成后调用：若 vpnv4 已使能，补推该 head 到 pending
 *
 * 由 bgp_calc_route_select() 末尾调用(与 bgp_import_rib_on_calc_done 并列)。
 * @param src_inst 源 instance(应为私网 VRF 的 ipv4 unicast)
 * @param head     发生 best 变化的 rthead
 */
void bgp_vrf_export_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head);

/**
 * @brief 某私网 VRF 配置 RD 后调用：把该 VRF 已有 ipv4-unicast 路由补灌导出到 vpnv4
 *
 * 用于"vpnv4 先使能、私网 VRF 后配 RD"的时序:enable 全量扫描时该 VRF 因无 RD 被跳过,
 * RD 配上来后由本函数把它已有的路由扫入 pending 重新导出。vpnv4 未使能时为 no-op。
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

/**
 * @brief 私网 VRF 的 ipv4-unicast instance 销毁前调用：撤销其名下所有已导出 VPN 路由
 *
 * 必须在源 instance 的 RIB 释放前调用(此时源 route 节点仍存活)，否则导出节点持有的
 * borrow 引用会让源节点滞留为 STALE 孤儿且 VPN 路由残留。
 * 由 bgp_instance_destroy() 在 (私网 VRF, ipv4, unicast) 实例上调用。
 * @param src_inst 即将销毁的源 instance
 */
void bgp_vrf_export_purge_source_inst(bgp_instance_t *src_inst);

/**
 * @brief 批量处理 vpnv4 inst 的 pending 队列(每次最多 batch 条)
 *
 * 由 worker BGP_WORKER_EVENT_VRF_EXPORT 事件回调驱动。
 * @return 实际处理条目数
 */
int bgp_vrf_export_queue_process(bgp_instance_t *vpn_inst, int batch);

/**
 * @brief 同步抽干 vpnv4 inst 的 pending 队列(drain_pending / 关闭路径)
 * @return 实际处理条目数
 */
int bgp_vrf_export_process_pending(bgp_instance_t *vpn_inst);

#endif /* BGP_VRF_EXPORT_H */
