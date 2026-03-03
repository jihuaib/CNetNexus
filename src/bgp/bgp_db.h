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

/**
 * @brief 查询全部 BGP 配置
 * @param ctx    BGP 模块的 IPC 上下文
 * @param result 输出结果集（调用者须通过 db_result_free 释放）
 * @return 0 成功，-1 失败
 */
int bgp_db_query(dev_ipc_context_t *ctx, db_result_t **result);

// ============================================================================
// BGP Session 操作（BGP 视图 neighbor 命令）
// ============================================================================

/**
 * @brief 插入或更新 BGP 会话（neighbor <ip> as <as-num>）
 * @param ctx         BGP 模块的 IPC 上下文
 * @param vrf         VRF 名称（NULL 时使用 BGP_VRF_DEFAULT）
 * @param neighbor_ip 邻居 IP 地址字符串
 * @param remote_as   远端 AS 号
 * @return 0 成功，-1 失败
 */
int bgp_db_set_session(dev_ipc_context_t *ctx, const char *vrf, const char *neighbor_ip, uint32_t remote_as);

/**
 * @brief 删除 BGP 会话
 * @param ctx         BGP 模块的 IPC 上下文
 * @param vrf         VRF 名称（NULL 则不限 VRF）
 * @param neighbor_ip 邻居 IP 地址字符串（为 NULL 则删除全部匹配行）
 * @return 删除的行数，错误返回 -1
 */
int bgp_db_del_session(dev_ipc_context_t *ctx, const char *vrf, const char *neighbor_ip);

/**
 * @brief 查询所有 BGP 会话
 * @param ctx    BGP 模块的 IPC 上下文
 * @param result 输出结果集（调用者须通过 db_result_free 释放）
 * @return 0 成功，-1 失败
 */
int bgp_db_query_sessions(dev_ipc_context_t *ctx, db_result_t **result);

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

/**
 * @brief 查询所有地址族邻居
 * @param ctx    BGP 模块的 IPC 上下文
 * @param result 输出结果集（调用者须通过 db_result_free 释放）
 * @return 0 成功，-1 失败
 */
int bgp_db_query_neighbors(dev_ipc_context_t *ctx, db_result_t **result);

#endif /* BGP_DB_H */
