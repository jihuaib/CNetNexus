/**
 * @file   bgp_db.h
 * @brief  BGP 模块数据库操作接口（封装 db_rpc 调用）
 * @author jhb
 * @date   2026/02/23
 */

#ifndef BGP_DB_H
#define BGP_DB_H

#include <stdint.h>

#include "db.h"
#include "ipc.h"

/**
 * @brief 初始化 BGP 数据库（建表，如已存在则跳过）
 * @param ctx BGP 模块的 IPC 上下文
 * @return 0 成功，-1 失败
 */
int bgp_db_init(ipc_context_t *ctx);

/**
 * @brief 写入（插入或更新）BGP AS 号到数据库
 * @param ctx       BGP 模块的 IPC 上下文
 * @param as_number AS 号（1-4294967295）
 * @return 0 成功，-1 失败
 */
int bgp_db_set_as(ipc_context_t *ctx, uint32_t as_number);

/**
 * @brief 从数据库删除 BGP 配置
 * @param ctx        BGP 模块的 IPC 上下文
 * @param as_number  AS 号（仅当 has_as 为 1 时有效）
 * @param has_as     1 表示指定 AS 号删除；0 表示删除全部
 * @return 删除的行数，错误返回 -1
 */
int bgp_db_del_as(ipc_context_t *ctx, uint32_t as_number, int has_as);

/**
 * @brief 查询全部 BGP 配置
 * @param ctx    BGP 模块的 IPC 上下文
 * @param result 输出结果集（调用者须通过 db_result_free 释放）
 * @return 0 成功，-1 失败
 */
int bgp_db_query(ipc_context_t *ctx, db_result_t **result);

#endif /* BGP_DB_H */
