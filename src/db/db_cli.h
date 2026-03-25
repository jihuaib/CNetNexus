/**
 * @file   db_cli.h
 * @brief  数据库模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef DB_CLI_H
#define DB_CLI_H

#include <stdint.h>

#include "cli.h"
#include "dev.h"

/**
 * @brief 响应输出结构
 */
typedef struct db_cli_resp_out
{
    char message[CLI_MAX_RESP_LEN]; /**< CLI 输出缓冲 */
    int success;
    uint32_t has_more;     /**< 是否有更多数据 */
    uint32_t batch_offset; /**< 续传偏移量 */
} db_cli_resp_out_t;

/** DB CLI group_id 定义（与 commands.xml 中 group-id 一致） */
#define DB_CLI_GROUP_ID_SHOW 1 /**< show db 命令 */

/**
 * @brief DB CLI 命令处理主入口
 * @param msg 来自 cfg 模块的 CLI 命令消息
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_cli_process_command(dev_ipc_message_t *msg);

/**
 * @brief 处理 CLI continue 消息
 * @param msg 来自 cfg 模块的 continue 消息
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_cli_handle_continue(dev_ipc_message_t *msg);
int db_cli_handle_show_config(dev_ipc_message_t *msg);

/**
 * @brief 清理 DB CLI 分片状态
 */
void db_cli_cleanup_state(void);

/**
 * @brief 处理动态候选值查询（CLI_MSG_TYPE_QUERY_CANDIDATES）
 *
 * 查询 SQLite 中所有用户表名，以 "name1\0name2\0\0" 格式响应。
 *
 * @param msg 原始 IPC 消息（函数负责释放）
 */
void db_cli_handle_query_candidates(dev_ipc_message_t *msg);

#endif // DB_CLI_H
