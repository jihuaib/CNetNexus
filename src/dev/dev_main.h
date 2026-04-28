/**
 * @file   dev_main.h
 * @brief  Dev 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef DEV_MAIN_H
#define DEV_MAIN_H

#include "cli.h"
#include "dev.h"

typedef struct dev_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    cli_chunk_stream_t show_stream; /**< CLI show 命令分片输出状态 */
} dev_local_t;

extern dev_local_t *g_dev_local;

/**
 * @brief DEV 自身初始化（创建 IPC context）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int dev_init_self(void);

/**
 * @brief 获取 DEV 的 IPC context
 * @return DEV 的 IPC context
 */
dev_ipc_context_t *dev_get_ipc_ctx(void);

/**
 * @brief DEV 清理
 */
void dev_cleanup_self(void);

/**
 * @brief 广播日志级别给所有已就绪模块（IPC 库层透明应用）
 * @param level 目标日志级别（log_level_t 取值）
 */
void dev_broadcast_log_level(uint32_t level);

#endif // DEV_MAIN_H
