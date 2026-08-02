/**
 * @file   bgp_vrf_import.h
 * @brief  VPNv4/VPNv6 路由按同 AF import-RT 导入私网 VRF，支持传统
 *         MPLS L3VPN 与整 SID 模式的 L3VPN over SRv6 BE。
 * @author jhb
 * @date   2026/06/02
 *
 * 设计要点：
 *  - IRT 索引：(afi, 规范化 8 字节 RT) → 私网 vrf_id 集合(带 refcount)。
 *    由 VRF 事件(VRF_EVENT_AF_IMPORT_RT_ADD/DEL)增量维护，VRF_DEL/AF_DISABLE 按 vrf 清理，
 *    VRF 进程重启(resync)时整体清空后由 REPLAY 重建。全部在 BGP worker 线程内操作。
 *  - 入向过滤：VPN route-target 只在 NLRI 所属 AF 的 IRT 中匹配。
 *  - 导入 reconcile：public VPN instance 的某 head 来源变化时，遍历各私网 VRF：
 *    命中 IRT 则把 best(剥 RD 成 unicast NLRI)作为合成导入路径写入该 VRF 的 unicast RIB，
 *    否则撤销该 (rd,prefix) 在该 VRF 的合成导入路径。
 *
 * 指针生命周期：导入节点(VRF unicast RIB 内、BGP_ROUTE_FLAG_IMPORT、合成来源=源 RD)的
 *  src_route 指向其来源 vpnv4 best 节点并 borrow_ref 钉住；撤销/best 切换/源 inst 销毁时
 *  统一在本子系统内 detach。与 bgp_vrf_export 的 borrow 约定一致。
 *
 *  - 转发：MPLS 路径迭代远端 PE tunnel 并携带 VPN label；SRv6 路径
 *    以 service SID 作为 public IPv6 nexthop 迭代对象，解析后下刷 SRv6 BE。
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
 * @brief 向 IRT 索引登记一条 (vrf_id, afi, import-RT)
 * @param vrf_id 私网 VRF ID(public 忽略)
 * @param afi    BGP_AFI_IPV4 / BGP_AFI_IPV6
 * @param rt     VRF 配置的 import RT(原始 8 字节)
 */
void bgp_vrf_import_irt_add(uint32_t vrf_id, bgp_afi_t afi, const vrf_rt_t *rt);

/**
 * @brief 从 IRT 索引注销一条 (vrf_id, import-RT)
 */
void bgp_vrf_import_irt_del(uint32_t vrf_id, bgp_afi_t afi, const vrf_rt_t *rt);

/**
 * @brief 从 IRT 索引移除某 VRF 的全部 import-RT(VRF_DEL / AF_DISABLE)
 */
void bgp_vrf_import_purge_vrf(uint32_t vrf_id);

/** @brief 从 IRT 索引仅移除某 VRF 指定 AF 的 import-RT */
void bgp_vrf_import_purge_vrf_afi(uint32_t vrf_id, bgp_afi_t afi);

/**
 * @brief 清空整个 IRT 索引(VRF 进程重启 resync，随后由 REPLAY 重建)
 */
void bgp_vrf_import_purge_all(void);

/**
 * @brief 入向过滤判据：该路由属性携带的 RT 是否命中任意私网 VRF 的 import-RT
 * @param attr 路由路径属性(含 ext-communities)
 * @param afi  收到路由的 VPN AFI
 * @return TRUE=至少一个 VRF 接受；FALSE=无任何同 AF 匹配
 */
gboolean bgp_vrf_import_attr_has_match(const bgp_attr_t *attr, bgp_afi_t afi);

/* ============================================================================
 * inter-AS Option B 中转换标（ASBR 转发面）
 *
 * ASBR 无 VRF、把收到的 vpnv4 路由改下一跳为本端再通告给上游时，必须给该路由分配一个本地入标签
 * 并装一条 MPLS SWAP 表项（本地入标签 → 换成收到的对端 VPN 标签 → 经下游隧道转发）。这两个 API
 * 负责本地标签的申请/释放（向 TUNNEL，TUNNEL 据 swap_label+endpoint 装 SWAP ILM 下到 FIB）。
 * ========================================================================== */

/**
 * @brief 为一条 vpnv4 中转路由申请本地入标签（用于改下一跳通告 + SWAP 转发），幂等缓存在节点上
 *
 * 要求 best 携带收到的 VPN 标签(has_label)且有可解析的 BGP 下一跳。成功后 best->out_local_label 缓存
 * 返回值，并向 TUNNEL 注册 SWAP 绑定(swap_label=收到的标签, endpoint=BGP 下一跳)。
 * @param best vpnv4 中转最优路径节点
 * @return 本地入标签(>0)；0=暂不可得(无标签/无下一跳/TUNNEL 不可用)，调用方应 hold 不通告
 */
uint32_t bgp_vrf_import_transit_alloc_label(bgp_route_node_t *best);

/**
 * @brief 释放某节点先前申请的中转本地入标签（节点回收时调用），并撤销对应 SWAP ILM
 * @param route 路由节点（out_local_label==0 时为 no-op）
 */
void bgp_vrf_import_transit_release_label(bgp_route_node_t *route);

/**
 * @brief public VPN instance 某 head best 变化后调用：把 best 导入命中的 VRF、
 *        撤销不再命中的 VRF。由 bgp_calc_route_select 末尾调用(与 export 并列)。
 * @param src_inst 源 instance(仅 public VRF, ipv4/ipv6, vpn-unicast 时生效)
 * @param head     发生 best 变化的 VPN rthead
 */
void bgp_vrf_import_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head);

/**
 * @brief import-RT 配置变更后，重新评估公网 vpnv4 RIB 中已有路由(补导入/补撤销)
 *
 * vpnv4 未使能时为 no-op。由 bgp_apply_vrf.c 在处理 IMPORT_RT_ADD/DEL 后调用。
 */
void bgp_vrf_import_backfill(void);

/** @brief 仅重评估指定 VPN AF 的已有路由 */
void bgp_vrf_import_backfill_afi(bgp_afi_t afi);

/**
 * @brief 只重评估一个私网 unicast instance 对已有 VPN 路由的导入状态
 *
 * 用于 srv6-be 运行态切换。已有 REMOTE_CROSS 节点会原地拆除旧
 * tunnel/SID watch，按 instance 的当前模式重建，并置 FIB_DIRTY 触发替换或撤销。
 * @param uc 目标私网 IPv4/IPv6 unicast instance
 * @return 0 成功（公网 VPN AF 未使能时也是成功 no-op）；非 0 参数无效
 */
int bgp_vrf_import_reprocess_target_inst(bgp_instance_t *uc);

/**
 * @brief 收到一条 VPNv4/VPNv6 reach 路由并写入公网 VPN RIB 后触发导入评估
 *
 * L3VPN 的 vpnv4 下一跳是远端 PE，无 LSP 时收到的路由恒为 invalid，不会产生 best 变化、
 * 因而不触发 calc/on_calc_done。此函数显式把该 vpnv4 NLRI 推入 calc 队列，使 calc_run_one
 * 走 all-invalid 分支也能调用 reconcile 完成按 import-RT 导入。由 bgp_relay 在 ingest 成功后调用。
 * @param vpn_nlri 收到的 VPN NLRI(afi=ipv4/ipv6, safi=vpn-unicast)
 */
void bgp_vrf_import_on_vpn_received(const bgp_nlri_entry_t *vpn_nlri);

/**
 * @brief import-RT 配置变更后，向所有已建立的 VPNv4/VPNv6 邻居发送 ROUTE-REFRESH(RFC 2918)
 *
 * 入向过滤会丢弃不命中的 VPN 路由(无 retain route-target all)，故新增 import-RT 后需让对端
 * 重传，本端再以新 IRT 索引重新评估接收。仅对协商了 Route Refresh 能力的 Established 邻居发送。
 * 对应 VPN AF 未使能时为 no-op。由 bgp_apply_vrf.c 在处理 IMPORT_RT_ADD 后调用。
 */
void bgp_vrf_import_request_refresh(void);

/** @brief 仅向指定 VPN AF 的已建立邻居发送 ROUTE-REFRESH */
void bgp_vrf_import_request_refresh_afi(bgp_afi_t afi);

/**
 * @brief public VPN instance 销毁前调用：撤销其名下所有已导入到各 VRF 的合成路径
 *
 * 必须在 VPN instance 的 RIB 释放前调用(此时源 VPN 节点仍存活)，否则导入节点持有的
 * borrow 引用会让源节点滞留为 STALE 孤儿。由 bgp_instance_destroy() 调用。
 * @param vpn_inst 即将销毁的 public VPN instance
 */
void bgp_vrf_import_purge_target_inst(bgp_instance_t *vpn_inst);

/* ============================================================================
 * VRF 本地交叉（local route leaking）：本机 VRF→VRF 直接泄漏，复用 IRT 索引
 *
 * 与上面的 vpnv4 导入对称但「源在本机」：某私网 VRF 的 ipv4-unicast best 按该 VRF 的
 * export-RT 直接命中其它私网 VRF 的 import-RT(IRT 索引)，命中即把该前缀作为合成路径
 * (BGP_ROUTE_FLAG_LOCAL_CROSS、目标路由自有 nexthop 对象在源 VRF 内迭代)插入目标 VRF 的 unicast RIB。
 * 完全独立于 vpnv4；单跳不传递(LOCAL_CROSS/REMOTE_CROSS 不再作泄漏源)。
 * ========================================================================== */

/**
 * @brief 私网 VRF ipv4-unicast 某 head best 变化后调用：按源 VRF export-RT 直接泄漏到命中的本机 VRF
 *
 * 由 bgp_calc_route_select 末尾调用(与 vpnv4 import/export 并列)。源非私网 ipv4-unicast 时为 no-op。
 * @param src_inst 源 instance(私网 VRF, ipv4, unicast)
 * @param head     发生 best 变化的 unicast rthead
 */
void bgp_vrf_import_local_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head);

/**
 * @brief 某 VRF 的 import-RT 配置变更后：重扫所有私网 VRF unicast RIB，补/撤本地泄漏
 *
 * 新增 import-RT → 其它 VRF 已有路由若 export-RT 命中则补泄漏进本 VRF；删除则撤销不再命中的。
 * 由 bgp_apply_vrf.c 处理 IMPORT_RT_ADD/DEL(ipv4-unicast) 后调用。
 * @param tgt_vrf_id 发生 import-RT 变更的 VRF(此处仅作日志/语义标识，实际全量重评)
 */
void bgp_vrf_import_local_backfill_target_vrf(uint32_t tgt_vrf_id);

/**
 * @brief 某 VRF 的 export-RT 配置变更后：重扫该 VRF unicast RIB，按新 export-RT 补/撤本地泄漏
 *
 * 由 bgp_apply_vrf.c 处理 EXPORT_RT_ADD/DEL(ipv4-unicast) 后调用。
 * @param src_vrf_id 发生 export-RT 变更的源 VRF
 */
void bgp_vrf_import_local_backfill_source_vrf(uint32_t src_vrf_id);

/**
 * @brief 私网 VRF ipv4-unicast instance 销毁前调用：双角色清理本地泄漏路径
 *
 * (a) 作为源：撤销其它 VRF 中由本 inst 泄漏出去的合成路径(解除 borrow，本 inst RIB 尚存活)；
 * (b) 作为目标：对本 inst RIB 内的泄漏合成路径 detach 来源 borrow。
 * 必须在本 inst 的 RIB 释放前调用。由 bgp_instance_destroy() 调用。
 * @param inst 即将销毁的私网 ipv4-unicast instance
 */
void bgp_vrf_import_local_purge_inst(bgp_instance_t *inst);

#endif /* BGP_VRF_IMPORT_H */
