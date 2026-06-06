/**
 * @file   bgp_vrf_import.h
 * @brief  vpnv4 路由按 import-RT 导入私网 VRF：BGP 维护全量 import-RT(IRT) 索引，
 *         收到 vpnv4 路由时若无 IRT 匹配则丢弃，匹配则把私网前缀(剥 RD)灌入对应 VRF
 *         的 ipv4-unicast RIB。与 bgp_vrf_export 方向相反、相互对称。
 * @author jhb
 * @date   2026/06/02
 *
 * 设计要点：
 *  - IRT 索引：规范化 8 字节 RT → 引用该 RT 的私网 vrf_id 集合(带 refcount)。
 *    由 VRF 事件(VRF_EVENT_AF_IMPORT_RT_ADD/DEL)增量维护，VRF_DEL/AF_DISABLE 按 vrf 清理，
 *    VRF 进程重启(resync)时整体清空后由 REPLAY 重建。全部在 BGP worker 线程内操作。
 *  - 入向过滤：bgp_relay 收到 vpnv4 reach NLRI 时，先用本路由属性里的 RT 查 IRT 索引；
 *    无任何匹配 → 整条丢弃(不入公网 vpnv4 RIB，也不导入任何 VRF)。匹配 → 正常入 vpnv4 RIB，
 *    导入由 vpnv4 calc 完成后的 reconcile 完成(保留 vpnv4 副本 + 导入 VRF)。
 *  - 导入 reconcile：public vpnv4 instance 的某 head best 变化时，遍历各私网 VRF：
 *    命中 IRT 则把 best(剥 RD 成 unicast NLRI)作为合成导入路径写入该 VRF 的 unicast RIB，
 *    否则撤销该 (rd,prefix) 在该 VRF 的合成导入路径。
 *
 * 指针生命周期：导入节点(VRF unicast RIB 内、BGP_ROUTE_FLAG_IMPORT、合成来源=源 RD)的
 *  src_route 指向其来源 vpnv4 best 节点并 borrow_ref 钉住；撤销/best 切换/源 inst 销毁时
 *  统一在本子系统内 detach。与 bgp_vrf_export 的 borrow 约定一致。
 *
 * 注意：导入节点的转发(基于 VPN 标签的隧道下一跳迭代)属后续工作，当前仅把路由正确灌入
 *       VRF Loc-RIB 并携带 received 标签，符合 vpnv4 子系统当前成熟度(与 export 发送编码器
 *       尚未完成相对应)。
 */
#ifndef BGP_VRF_IMPORT_H
#define BGP_VRF_IMPORT_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "bgp_instance.h"
#include "bgp_rib.h"
#include "vrf.h"

/**
 * @brief 初始化 IRT 索引(BGP worker 启动早期调用一次)
 */
void bgp_vrf_import_init(void);

/**
 * @brief 销毁 IRT 索引(worker 关闭路径)
 */
void bgp_vrf_import_cleanup(void);

/**
 * @brief 向 IRT 索引登记一条 (vrf_id, import-RT)
 * @param vrf_id 私网 VRF ID(public 忽略)
 * @param rt     VRF 配置的 import RT(原始 8 字节)
 */
void bgp_vrf_import_irt_add(uint32_t vrf_id, const vrf_rt_t *rt);

/**
 * @brief 从 IRT 索引注销一条 (vrf_id, import-RT)
 */
void bgp_vrf_import_irt_del(uint32_t vrf_id, const vrf_rt_t *rt);

/**
 * @brief 从 IRT 索引移除某 VRF 的全部 import-RT(VRF_DEL / AF_DISABLE)
 */
void bgp_vrf_import_purge_vrf(uint32_t vrf_id);

/**
 * @brief 清空整个 IRT 索引(VRF 进程重启 resync，随后由 REPLAY 重建)
 */
void bgp_vrf_import_purge_all(void);

/**
 * @brief 入向过滤判据：该路由属性携带的 RT 是否命中任意私网 VRF 的 import-RT
 * @param attr 路由路径属性(含 ext-communities)
 * @return TRUE=至少一个 VRF 接受；FALSE=无任何匹配(调用方应丢弃该 vpnv4 路由)
 */
gboolean bgp_vrf_import_attr_has_match(const bgp_attr_t *attr);

/**
 * @brief public vpnv4 instance 某 head best 变化后调用：把 best 导入命中的 VRF、
 *        撤销不再命中的 VRF。由 bgp_calc_route_select 末尾调用(与 export 并列)。
 * @param src_inst 源 instance(仅 public VRF, ipv4, vpn-unicast 时生效)
 * @param head     发生 best 变化的 vpnv4 rthead
 */
void bgp_vrf_import_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head);

/**
 * @brief import-RT 配置变更后，重新评估公网 vpnv4 RIB 中已有路由(补导入/补撤销)
 *
 * vpnv4 未使能时为 no-op。由 bgp_apply_vrf.c 在处理 IMPORT_RT_ADD/DEL 后调用。
 */
void bgp_vrf_import_backfill(void);

/**
 * @brief 收到一条 vpnv4 reach 路由(已通过 IRT 过滤)并写入公网 vpnv4 RIB 后调用：触发导入评估
 *
 * L3VPN 的 vpnv4 下一跳是远端 PE，无 LSP 时收到的路由恒为 invalid，不会产生 best 变化、
 * 因而不触发 calc/on_calc_done。此函数显式把该 vpnv4 NLRI 推入 calc 队列，使 calc_run_one
 * 走 all-invalid 分支也能调用 reconcile 完成按 import-RT 导入。由 bgp_relay 在 ingest 成功后调用。
 * @param vpn_nlri 收到的 vpnv4 NLRI(afi=ipv4 safi=vpn-unicast)
 */
void bgp_vrf_import_on_vpn_received(const bgp_nlri_entry_t *vpn_nlri);

/**
 * @brief import-RT 配置变更后，向所有已建立的 vpnv4 邻居发送 ROUTE-REFRESH(RFC 2918)
 *
 * 入向过滤会丢弃不命中的 vpnv4 路由(无 retain route-target all)，故新增 import-RT 后需让对端
 * 重传，本端再以新 IRT 索引重新评估接收。仅对协商了 Route Refresh 能力的 Established 邻居发送。
 * vpnv4 未使能时为 no-op。由 bgp_apply_vrf.c 在处理 IMPORT_RT_ADD 后调用。
 */
void bgp_vrf_import_request_refresh(void);

/**
 * @brief public vpnv4 instance 销毁前调用：撤销其名下所有已导入到各 VRF 的合成路径
 *
 * 必须在 vpnv4 instance 的 RIB 释放前调用(此时源 vpnv4 节点仍存活)，否则导入节点持有的
 * borrow 引用会让源节点滞留为 STALE 孤儿。由 bgp_instance_destroy() 调用。
 * @param vpn_inst 即将销毁的 public vpnv4 instance
 */
void bgp_vrf_import_purge_target_inst(bgp_instance_t *vpn_inst);

#endif /* BGP_VRF_IMPORT_H */
