/**
 * @file   dev_cli.h
 * @brief  Dev 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef DEV_CLI_H
#define DEV_CLI_H

#include "cli.h"
#include "dev.h"

// 命令组 ID（对应 commands.xml）
#define DEV_CLI_GROUP_ID_SHOW_VERSION 1
#define DEV_CLI_GROUP_ID_SYSNAME 2
#define DEV_CLI_GROUP_ID_SHOW_MODULE 3
#define DEV_CLI_GROUP_ID_LOG_LEVEL 4
#define DEV_CLI_GROUP_ID_PING 5
#define DEV_CLI_GROUP_ID_SHOW_IPC 6
#define DEV_CLI_GROUP_ID_REBOOT 7

typedef struct dev_cli_out
{
    uint32_t group_id;
} dev_cli_out_t;

/**
 * @brief 响应输出结构
 */
typedef struct dev_cli_resp_out
{
    char message[CLI_MAX_RESP_LEN]; /**< CLI 输出缓冲 */
    int success;
    uint32_t has_more;     /**< 是否有更多数据 */
    uint32_t batch_offset; /**< 续传偏移量 */
} dev_cli_resp_out_t;

int dev_cli_handle_message(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int dev_cli_handle_continue(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int dev_cli_handle_show_config(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
void dev_cli_cleanup_state(void);
void dev_cli_handle_query_candidates(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

#endif // DEV_CLI_H
