/**
 * @file   bgp_cfg_apply.h
 * @brief  BGP 配置内存态应用接口（校验 + 短路 + 内存更新 + 副作用，结果写入 apply->rc/errmsg）
 * @author jhb
 * @date   2026/03/07
 */
#ifndef BGP_CFG_APPLY_H
#define BGP_CFG_APPLY_H

/* 前向声明，避免循环包含 */
struct bgp_apply_cmd;
typedef struct bgp_apply_cmd bgp_apply_cmd_t;

/**
 * @brief 应用 bgp/no bgp 到内存状态（创建或销毁 bgp_protocol_t，启停监听）
 */
void bgp_cfg_apply_protocol(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 BGP VRF 视图入口到内存（创建 BGP 侧 VRF 容器）
 */
void bgp_cfg_apply_vrf(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 neighbor/no neighbor（BGP 视图）到内存
 */
void bgp_cfg_apply_neighbor(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 address-family/no address-family 到内存
 */
void bgp_cfg_apply_instance(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 AF 视图 neighbor enable/no neighbor 到内存
 */
void bgp_cfg_apply_af_neighbor(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 router-id/no router-id 到内存（变更后重置所有会话）
 */
void bgp_cfg_apply_router_id(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 timers/no timers 到内存（变更后重置所有会话）
 */
void bgp_cfg_apply_timers(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 connect-retry/no connect-retry 到内存（变更后重排 retry 定时器）
 */
void bgp_cfg_apply_connect_retry(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 open-capability/no open-capability 到内存（变更后重置会话）
 */
void bgp_cfg_apply_open_cap(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 import-route/no import-route 到内存
 */
void bgp_cfg_apply_import_route(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 neighbor source-interface/no neighbor source-interface 到内存
 */
void bgp_cfg_apply_source_if(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 neighbor ebgp-multihop/no neighbor ebgp-multihop 到内存
 */
void bgp_cfg_apply_ebgp_multihop(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 QP 自产生路由配置/删除：注入/撤销 NLRI，维护 inst->qp_routes
 */
void bgp_cfg_apply_qp_route(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 QP 地址族 route-select enable 开关
 */
void bgp_cfg_apply_route_select(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 VPN 地址族 policy vpn-target 入向过滤开关（默认启用；no 清除实例标志位）
 *
 * 切换后对 VPN 类实例(public，按 apply 携带 afi/safi 定位)置/清 BGP_INST_FLAG_VPN_TARGET_FILTER，
 * 并触发 vpnv4 ROUTE-REFRESH 让对端重传以按新策略重新评估接收。
 */
void bgp_cfg_apply_vpn_target_policy(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 advertise evpn route / no advertise evpn route（私网 VRF unicast AF）
 */
void bgp_cfg_apply_advertise_evpn_route(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 refresh bgp 命令：import 向对端发 ROUTE-REFRESH，export 本端重发 Adj-RIB-Out
 */
void bgp_cfg_apply_refresh(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 reflector cluster-id / no reflector cluster-id（RFC 4456）
 */
void bgp_cfg_apply_cluster_id(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 neighbor reflect-client / no neighbor reflect-client（AF 视图）
 */
void bgp_cfg_apply_reflect_client(bgp_apply_cmd_t *apply);

/** 应用邻居出口策略绑定。 */
void bgp_cfg_apply_export_policy(bgp_apply_cmd_t *apply);

#endif /* BGP_CFG_APPLY_H */
