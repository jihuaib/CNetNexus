/**
 * @file   sbmp_main.h
 * @brief  SBMP（BMP Server）模块主入口头文件
 * @author jhb
 * @date   2026/03/08
 */
#ifndef SBMP_MAIN_H
#define SBMP_MAIN_H

#include "dev.h"

/**
 * @brief SBMP 模块本地状态
 */
typedef struct sbmp_local
{
    dev_ipc_context_t *dev_ipc_ctx; /**< IPC 上下文 */
    uint16_t server_port;           /**< BMP 监听端口（0 = 未配置） */
    int listen_fd;                  /**< BMP TCP 监听 fd（-1 = 未监听） */
} sbmp_local_t;

/** SBMP 模块全局状态 */
extern sbmp_local_t *g_sbmp_local;

/**
 * @brief IPC 消息处理回调
 */
void sbmp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief 启动 BMP server 监听（绑定指定端口），已监听时幂等
 * @param port 监听端口
 */
void sbmp_listen_start(uint16_t port);

/**
 * @brief 停止 BMP server 监听，未监听时幂等
 */
void sbmp_listen_stop(void);

#endif /* SBMP_MAIN_H */
