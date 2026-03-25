/**
 * @file   if_main.h
 * @brief  接口模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef IF_MAIN_H
#define IF_MAIN_H

#include <glib.h>

#include "cli.h"
#include "dev.h"
#include "if_map.h"

typedef struct
{
    dev_ipc_context_t *dev_ipc_ctx;
    cli_chunk_stream_t show_stream; /**< CLI show 命令分片输出状态 */
    if_map_t interface_map;         /**< 接口逻辑名到物理名的映射表 */
    GList *subscribers;             /**< IF 事件订阅者列表 GList<if_subscriber_t*> */
} if_local_t;

extern if_local_t *g_if_local;

/**
 * @brief 获取 IF 模块本地 IPC 上下文（架构保证非空）
 */
static inline dev_ipc_context_t *if_local_ipc_ctx(void)
{
    return g_if_local->dev_ipc_ctx;
}

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void if_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief IF 模块初始化（由 if_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int if_module_init(void);

#endif // IF_MAIN_H
