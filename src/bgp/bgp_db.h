/**
 * @file   bgp_db.h
 * @brief  BGP 模块数据库操作接口（封装 db_rpc 调用）
 * @author jhb
 * @date   2026/02/23
 */

#ifndef BGP_DB_H
#define BGP_DB_H

#include <stdbool.h>
#include <stdint.h>

#include "bgp_protocol.h"
#include "db.h"
#include "dev.h"

/** BGP 协议配置表名 */
#define BGP_TABLE_PROTOCOL "bgp_protocol"
/** BGP 会话表名 */
#define BGP_TABLE_SESSION "bgp_session"
/** BGP 邻居表名 */
#define BGP_TABLE_NEIGHBOR "bgp_neighbor"
/** BGP VRF 配置表名 */
#define BGP_TABLE_VRF "bgp_vrf"
/** BGP 地址族实例表名 */
#define BGP_TABLE_INSTANCE "bgp_instance"
/** BGP QP 自产生路由配置表名 */
#define BGP_TABLE_QP_ROUTE "bgp_qp_route"

/**
 * @brief 从数据库恢复 BGP 协议内存状态
 * @return 恢复的协议结构指针（调用者持有，最终由 bgp_protocol_destroy 释放），无配置时返回 NULL
 */
uint32_t bgp_db_restore(void);

/**
 * @brief VRF 重启 re-sync：只重恢复 vrf_name 非 public 的行（vrf/session/instance/neighbor/qp_route）。
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
uint32_t bgp_db_restore_vrf_bound(void);

/**
 * @brief 初始化 BGP 数据库（建表，如已存在则跳过）
 * @return 0 成功，-1 失败
 */
int bgp_db_init(void);

/**
 * @brief 确保数据库有默认配置（表为空时写入默认值，非空时跳过）
 *
 * 适用于两种场景：首次启动（表刚创建）和用户删除全部配置后重启。
 */
void bgp_db_ensure_defaults(void);

/**
 * @brief 写入（插入或更新）BGP AS 号到数据库
 * @param as_number AS 号（1-4294967295）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_as(uint32_t as_number);

/**
 * @brief 从数据库删除 BGP 协议及关联配置（protocol/session/neighbor/instance/qp_route/vrf/bmp_instance/bmp_monitor）
 * @return 删除的行数，错误返回 -1
 */
int bgp_db_del_as(void);

// ============================================================================
// BGP Session 操作（BGP 视图 neighbor 命令）
// ============================================================================

/**
 * @brief 插入或更新 BGP 会话（neighbor <ip> as <as-num>）
 * @param vrf_name    VRF 名称（public 为默认公网 VRF）
 * @param neighbor_ip 邻居 IP 地址字符串
 * @param remote_as   远端 AS 号
 * @return 0 成功，-1 失败
 */
int bgp_db_set_session(const char *vrf_name, const char *neighbor_ip, uint32_t remote_as);

/**
 * @brief 删除 BGP 会话，并级联删除对应 neighbor(AF 使能) 记录
 * @param vrf_name    VRF 名称（public 为默认公网 VRF）
 * @param neighbor_ip 邻居 IP 地址字符串（为 NULL 则删除该 VRF 内所有 session 和 neighbor）
 * @return 删除的行数，错误返回 -1
 */
int bgp_db_del_session(const char *vrf_name, const char *neighbor_ip);

/**
 * @brief 更新指定 session 的 OPEN 能力标记位（BGP_SESS_CAP_*）
 * @param vrf_name    VRF 名称（public 为默认公网 VRF）
 * @param neighbor_ip 邻居 IP 地址字符串
 * @param open_caps   能力标记位值（BGP_SESS_CAP_AS4 | BGP_SESS_CAP_ROUTE_REFRESH 等）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_session_caps(const char *vrf_name, const char *neighbor_ip, uint32_t open_caps);

/**
 * @brief 设置 session 的 source-interface
 * @param vrf_name    VRF 名称
 * @param neighbor_ip 邻居 IP 地址
 * @param if_name     source-interface 逻辑接口名
 * @return 0 成功，-1 失败
 */
int bgp_db_set_session_source_if(const char *vrf_name, const char *neighbor_ip, const char *if_name);

/**
 * @brief 清除 session 的 source-interface（置空字符串）
 * @param vrf_name    VRF 名称
 * @param neighbor_ip 邻居 IP 地址
 * @return 0 成功，-1 失败
 */
int bgp_db_del_session_source_if(const char *vrf_name, const char *neighbor_ip);

/**
 * @brief 设置 session 的 ebgp-multihop TTL
 * @param vrf_name    VRF 名称
 * @param neighbor_ip 邻居 IP 地址
 * @param ttl         TTL（1-255）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_session_ebgp_multihop(const char *vrf_name, const char *neighbor_ip, uint8_t ttl);

/**
 * @brief 清除 session 的 ebgp-multihop（置 0）
 * @param vrf_name    VRF 名称
 * @param neighbor_ip 邻居 IP 地址
 * @return 0 成功，-1 失败
 */
int bgp_db_del_session_ebgp_multihop(const char *vrf_name, const char *neighbor_ip);

// ============================================================================
// BGP Neighbor 操作（地址族视图 neighbor enable 命令）
// ============================================================================

/**
 * @brief 使能地址族邻居（neighbor <ip> enable）
 * @param vrf_name    VRF 名称（public 为默认公网 VRF）
 * @param neighbor_ip 邻居 IP 地址字符串
 * @param afi         地址族（bgp_afi_t）
 * @param safi        子地址族（bgp_safi_t）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_neighbor(const char *vrf_name, const char *neighbor_ip, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 删除地址族邻居
 * @param vrf_name    VRF 名称（public 为默认公网 VRF）
 * @param neighbor_ip 邻居 IP 地址字符串
 * @param afi         地址族（bgp_afi_t）
 * @param safi        子地址族（bgp_safi_t）
 * @return 删除的行数，错误返回 -1
 */
int bgp_db_del_neighbor(const char *vrf_name, const char *neighbor_ip, bgp_afi_t afi, bgp_safi_t safi);

// ============================================================================
// BGP VRF 操作（VRF 级配置，如 router-id）
// ============================================================================

/**
 * @brief 确保 BGP VRF 配置行存在（默认 router-id/timer/connect-retry）
 * @param vrf_name VRF 名称
 * @return 0 成功，-1 失败
 */
int bgp_db_ensure_vrf(const char *vrf_name);

/**
 * @brief 删除 BGP VRF 及该 VRF 下的 session/neighbor/instance/自产生路由配置
 * @param vrf_name VRF 名称（public 不允许删除）
 * @return 成功返回删除行数，失败返回 -1
 */
int bgp_db_del_vrf(const char *vrf_name);

/**
 * @brief 设置 VRF 的 router-id（upsert）
 * @param vrf_name  VRF 名称（public 为默认公网 VRF）
 * @param router_id Router ID 字符串（点分十进制）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_vrf_router_id(const char *vrf_name, const char *router_id);

/**
 * @brief 将 VRF 的 router-id 配置重置为默认值（0.0.0.0）
 * @param vrf_name VRF 名称
 * @return 删除行数，-1 失败
 */
int bgp_db_del_vrf_router_id(const char *vrf_name);

/**
 * @brief 设置 VRF 定时器（upsert）
 * @param vrf_name  VRF 名称（public 为默认公网 VRF）
 * @param keepalive keepalive 时间（秒）
 * @param hold_time hold time（秒），须大于 keepalive
 * @return 0 成功，-1 失败
 */
int bgp_db_set_vrf_timers(const char *vrf_name, uint16_t keepalive, uint16_t hold_time);

/**
 * @brief 将 VRF 定时器重置为默认值（keepalive=60, hold=180）
 * @param vrf_name VRF 名称
 * @return 0 成功，-1 失败
 */
int bgp_db_del_vrf_timers(const char *vrf_name);

// ============================================================================
// BGP 地址族实例操作（bgp_instance 表）
// ============================================================================

/**
 * @brief 写入 AF 实例记录（幂等，已存在则跳过）
 * @param vrf_name VRF 名称
 * @param afi    地址族（bgp_afi_t）
 * @param safi   子地址族（bgp_safi_t）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_instance(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 删除 AF 实例记录，并级联删除该 AF 下所有 neighbor 使能记录
 * @param vrf_name VRF 名称
 * @param afi    地址族
 * @param safi   子地址族
 * @return 删除总行数（instance + neighbor + qp_route），-1 失败
 */
int bgp_db_del_instance(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 更新 AF 实例的 import_protos 位掩码
 * @param vrf_name      VRF 名称
 * @param afi           地址族
 * @param safi          子地址族
 * @param import_protos 已导入协议位掩码（bit N = protocol N 已启用）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_import_protos(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, uint32_t import_protos);

/**
 * @brief 删除指定地址族下的所有邻居使能记录（no af 时批量清理）
 * @param vrf_name VRF 名称（public 为默认公网 VRF）
 * @param afi    地址族（bgp_afi_t）
 * @param safi   子地址族（bgp_safi_t）
 * @return 删除行数，-1 失败
 */
int bgp_db_del_neighbors_by_afi(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 设置 QP 地址族 route-select enable 开关的持久化状态
 * @param vrf_name VRF 名称
 * @param afi     地址族
 * @param safi    子地址族（必为 BGP_SAFI_QP）
 * @param enabled TRUE=启用，FALSE=关闭
 * @return 0 成功，-1 失败
 */
int bgp_db_set_route_select(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, bool enabled);

/**
 * @brief 持久化一条 QP 自产生路由配置
 * @param vrf_name    VRF 名称
 * @param afi         地址族
 * @param safi        子地址族（必为 BGP_SAFI_QP）
 * @param start_dqpn  起始 DQPN
 * @param count       路由条数
 * @param prefix_addr 前缀基地址字符串
 * @param mask_len    前缀长度
 * @param bid         BID IPv6 地址字符串
 * @return 0 成功，-1 失败
 */
int bgp_db_set_qp_route(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, uint32_t start_dqpn, uint32_t count,
                        const char *prefix_addr, uint8_t mask_len, const char *bid);

/**
 * @brief 删除一条 QP 自产生路由配置
 * @param vrf_name    VRF 名称
 * @param afi         地址族
 * @param safi        子地址族（必为 BGP_SAFI_QP）
 * @param start_dqpn  起始 DQPN
 * @param count       路由条数
 * @param prefix_addr 前缀基地址字符串
 * @param mask_len    前缀长度
 * @param bid         BID IPv6 地址字符串
 * @return 删除行数，-1 失败
 */
int bgp_db_del_qp_route(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, uint32_t start_dqpn, uint32_t count,
                        const char *prefix_addr, uint8_t mask_len, const char *bid);

/**
 * @brief 删除指定地址族下的所有 QP 自产生路由配置（no af 时批量清理）
 * @param vrf_name VRF 名称
 * @param afi    地址族
 * @param safi   子地址族
 * @return 删除行数，-1 失败
 */
int bgp_db_del_qp_routes_by_afi(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi);

// ============================================================================
// BGP VRF 操作（connect-retry 定时器）
// ============================================================================

/**
 * @brief 设置 VRF 的 connect-retry 定时器（upsert）
 * @param vrf_name      VRF 名称（public 为默认公网 VRF）
 * @param connect_retry 主动连接失败后重试间隔（秒）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_vrf_connect_retry(const char *vrf_name, uint16_t connect_retry);

/**
 * @brief 将 VRF connect-retry 定时器重置为默认值（10 秒）
 * @param vrf_name VRF 名称
 * @return 0 成功，-1 失败
 */
int bgp_db_del_vrf_connect_retry(const char *vrf_name);

/**
 * @brief 设置 AF 实例的 Route Reflector Cluster-ID（RFC 4456，per-AF）
 */
int bgp_db_set_inst_cluster_id(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, uint32_t cluster_id);

/**
 * @brief 更新 AF 实例的 import-rib 源位掩码
 * @param vrf_name VRF 名称
 * @param afi     地址族
 * @param safi    子地址族
 * @param sources bgp_import_src_t 位掩码（bit N = 第 N 类源已启用）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_import_rib_sources(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, uint32_t sources);

/**
 * @brief 设置 AF 下邻居的 RR 客户端标记（RFC 4456）
 */
int bgp_db_set_neighbor_rr_client(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, const char *neighbor_ip,
                                  bool is_client);

#endif /* BGP_DB_H */
