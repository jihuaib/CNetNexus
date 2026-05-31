/**
 * @file   access_main.h
 * @brief  ACCESS 接入层（line 层）模块主入口头文件
 * @author jhb
 * @date   2026/05/30
 */
#ifndef ACCESS_MAIN_H
#define ACCESS_MAIN_H

#include "dev.h"

/**
 * @brief ACCESS 模块初始化（由 access_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int access_module_init(void);

/**
 * @brief ACCESS 模块清理（由 access_proc.c main() 退出前调用）
 */
void access_module_cleanup(void);

/**
 * @brief IPC 消息处理回调（处理 CLI→ACCESS 的响应/通知）
 * @param ctx IPC 上下文
 * @param msg 接收到的消息
 */
void access_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief 获取本模块 IPC 上下文（供 access_line 向 CLI 发 RPC）
 */
dev_ipc_context_t *access_ipc_ctx(void);

/**
 * @brief 按当前 vty transport 配置刷新 telnet(23) 监听：有 vty 开 telnet 则起监听，否则关。
 *        由 transport input 命令执行后调用（在 server 线程上下文）。
 */
void access_telnet_apply_gating(void);

#endif // ACCESS_MAIN_H
