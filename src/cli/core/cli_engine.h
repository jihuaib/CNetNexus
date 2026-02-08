/**
 * @file   cli_engine.h
 * @brief  CLI引擎公共API - 会话管理和命令处理
 * @author jhb
 * @date   2026/02/07
 */
#ifndef CLI_ENGINE_H
#define CLI_ENGINE_H

#include <stdint.h>
#include <pthread.h>
#include <glib.h>
#include "cli_handler.h"
#include "cli_history.h"
#include "cli_tree.h"
#include "cli_view.h"
#include "ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CLI引擎上下文结构
 */
// CLI引擎上下文结构
struct cli_engine_context {
    cli_view_tree_t view_tree;      // CLI视图树
    ipc_context_t *ipc_ctx;         // IPC上下文（用于模块通信）
    
    // 全局命令历史
    cli_global_history_t global_history;
    pthread_mutex_t history_mutex;
    
    // XML数据库定义列表
    GList *xml_db_defs;
    
    void *user_data;                // 用户自定义数据
};

// 全局CLI引擎上下文（用于cli_handler和cli_dispatch访问）
extern cli_engine_context_t *g_cli_engine_ctx;


/**
 * 初始化CLI引擎
 * @param module_id 当前模块ID（用于IPC）
 * @param module_name 模块名称
 * @return CLI引擎上下文，失败返回NULL
 */
cli_engine_context_t *cli_engine_init(uint32_t module_id, const char *module_name);

/**
 * 清理CLI引擎资源
 * @param ctx CLI引擎上下文
 */
void cli_engine_cleanup(cli_engine_context_t *ctx);

/**
 * 获取视图树（用于加载XML CLI定义）
 * @param ctx CLI引擎上下文
 * @return 视图树指针
 */
cli_view_tree_t *cli_engine_get_view_tree(cli_engine_context_t *ctx);

/**
 * 获取IPC上下文
 * @param ctx CLI引擎上下文
 * @return IPC上下文指针
 */
ipc_context_t *cli_engine_get_ipc_context(cli_engine_context_t *ctx);

/**
 * 设置用户数据
 * @param ctx CLI引擎上下文
 * @param user_data 用户数据指针
 */
void cli_engine_set_user_data(cli_engine_context_t *ctx, void *user_data);

/**
 * 获取用户数据
 * @param ctx CLI引擎上下文
 * @return 用户数据指针
 */
void *cli_engine_get_user_data(cli_engine_context_t *ctx);

/**
 * 创建CLI会话（由接入模块调用）
 * @param ctx CLI引擎上下文
 * @param client_fd 客户端文件描述符
 * @return CLI会话指针，失败返回NULL
 */
cli_session_t *cli_engine_create_session(cli_engine_context_t *ctx, int client_fd);

/**
 * 销毁CLI会话
 * @param session CLI会话指针
 */
void cli_engine_destroy_session(cli_session_t *session);

/**
 * 处理客户端输入（由接入模块在收到数据时调用）
 * @param session CLI会话指针
 * @return 0成功，-1表示会话应关闭
 */
int cli_engine_process_input(cli_session_t *session);

#ifdef __cplusplus
}
#endif

#endif // CLI_ENGINE_H
