/**
 * @file   if_show.h
 * @brief  IF worker 线程 show/continue/候选查询/分片输出接口
 * @author jhb
 * @date   2026/04/22
 */
#ifndef IF_SHOW_H
#define IF_SHOW_H

#include <glib.h>

#include "dev.h"

/**
 * @brief 处理 show interface CLI 命令（worker 线程内调用）
 */
int if_show_handle_cli(dev_ipc_message_t *msg);

/**
 * @brief 处理 CLI 分片续传请求（worker 线程）
 */
int if_show_handle_continue(dev_ipc_message_t *msg);

/**
 * @brief 处理动态候选值查询（worker 线程）
 */
void if_show_handle_query_candidates(dev_ipc_message_t *msg);

/**
 * @brief 通过分片流发送文本（worker 线程，供 show 与 show current-configuration 共用）
 * @param msg 原始请求
 * @param full_text 完整文本（接管所有权，可为 NULL）
 */
int if_show_send_chunked(dev_ipc_message_t *msg, GString *full_text);

/**
 * @brief 重置分片流状态（worker shutdown 时调用）
 */
void if_show_cleanup_state(void);

#endif /* IF_SHOW_H */
