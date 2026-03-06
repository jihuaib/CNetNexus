/**
 * @file   bgp_db.h
 * @brief  BGP 模块数据库操作接口（封装 db_rpc 调用）
 * @author jhb
 * @date   2026/02/23
 */

#ifndef BGP_DB_H
#define BGP_DB_H

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

/**
 * @brief 从数据库恢复 BGP 协议内存状态
 * @param ctx BGP 模块的 IPC 上下文
 * @return 恢复的协议结构指针（调用者持有，最终由 bgp_protocol_destroy 释放），无配置时返回 NULL
 */
bgp_protocol_t *bgp_db_restore(dev_ipc_context_t *ctx);

/**
 * @brief 初始化 BGP 数据库（建表，如已存在则跳过）
 * @param ctx BGP 模块的 IPC 上下文
 * @return 0 成功，-1 失败
 */
int bgp_db_init(dev_ipc_context_t *ctx);

/**
 * @brief 确保数据库有默认配置（表为空时写入默认值，非空时跳过）
 *
 * 适用于两种场景：首次启动（表刚创建）和用户删除全部配置后重启。
 * @param ctx BGP 模块的 IPC 上下文
 */
void bgp_db_ensure_defaults(dev_ipc_context_t *ctx);

/**
 * @brief 写入（插入或更新）BGP AS 号到数据库
 * @param ctx       BGP 模块的 IPC 上下文
 * @param as_number AS 号（1-4294967295）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_as(dev_ipc_context_t *ctx, uint32_t as_number);

/**
 * @brief 从数据库删除 BGP 配置
 * @param ctx        BGP 模块的 IPC 上下文
 * @return 删除的行数，错误返回 -1
 */
int bgp_db_del_as(dev_ipc_context_t *ctx);

// ============================================================================
// BGP Session 操作（BGP 视图 neighbor 命令）
// ============================================================================

/**
 * @brief 插入或更新 BGP 会话（neighbor <ip> as <as-num>）
 * @param ctx         BGP 模块的 IPC 上下文
 * @param vrf_id      VRF ID（0 为默认公网 VRF）
 * @param neighbor_ip 邻居 IP 地址字符串
 * @param remote_as   远端 AS 号
 * @return 0 成功，-1 失败
 */
int bgp_db_set_session(dev_ipc_context_t *ctx, uint32_t vrf_id, const char *neighbor_ip, uint32_t remote_as);

/**
 * @brief 删除 BGP 会话
 * @param ctx         BGP 模块的 IPC 上下文
 * @param vrf_id      VRF ID（0 为默认公网 VRF）
 * @param neighbor_ip 邻居 IP 地址字符串（为 NULL 则删除该 VRF 内所有 session）
 * @return 删除的行数，错误返回 -1
 */
int bgp_db_del_session(dev_ipc_context_t *ctx, uint32_t vrf_id, const char *neighbor_ip);

// ============================================================================
// BGP Neighbor 操作（地址族视图 neighbor enable 命令）
// ============================================================================

/**
 * @brief 使能地址族邻居（neighbor <ip> enable）
 * @param ctx         BGP 模块的 IPC 上下文
 * @param neighbor_ip 邻居 IP 地址字符串
 * @param afi         地址族标识（如 "ipv4-unicast"）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_neighbor(dev_ipc_context_t *ctx, const char *neighbor_ip, const char *afi);

/**
 * @brief 删除地址族邻居
 * @param ctx         BGP 模块的 IPC 上下文
 * @param neighbor_ip 邻居 IP 地址字符串
 * @param afi         地址族标识（如 "ipv4-unicast"），为 NULL 则删除该 IP 所有地址族
 * @return 删除的行数，错误返回 -1
 */
int bgp_db_del_neighbor(dev_ipc_context_t *ctx, const char *neighbor_ip, const char *afi);

// ============================================================================
// BGP VRF 操作（VRF 级配置，如 router-id）
// ============================================================================

/**
 * @brief 设置 VRF 的 router-id（upsert）
 * @param ctx       BGP 模块的 IPC 上下文
 * @param vrf_id    VRF ID（0 为默认公网 VRF）
 * @param router_id Router ID 字符串（点分十进制）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_vrf_router_id(dev_ipc_context_t *ctx, uint32_t vrf_id, const char *router_id);

/**
 * @brief 删除 VRF 的 router-id 配置
 * @param ctx    BGP 模块的 IPC 上下文
 * @param vrf_id VRF ID
 * @return 删除行数，-1 失败
 */
int bgp_db_del_vrf_router_id(dev_ipc_context_t *ctx, uint32_t vrf_id);

/**
 * @brief 设置 VRF 定时器（upsert）
 * @param ctx       BGP 模块的 IPC 上下文
 * @param vrf_id    VRF ID（0 为默认公网 VRF）
 * @param keepalive keepalive 时间（秒）
 * @param hold_time hold time（秒），须大于 keepalive
 * @return 0 成功，-1 失败
 */
int bgp_db_set_vrf_timers(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t keepalive, uint16_t hold_time);

/**
 * @brief 将 VRF 定时器重置为默认值（keepalive=60, hold=180）
 * @param ctx    BGP 模块的 IPC 上下文
 * @param vrf_id VRF ID
 * @return 0 成功，-1 失败
 */
int bgp_db_del_vrf_timers(dev_ipc_context_t *ctx, uint32_t vrf_id);

#endif /* BGP_DB_H */
