/**
 * @file   ipc_connection.h
 * @brief  IPC TCP 连接管理头文件
 * @author jhb
 * @date   2026/02/02
 */

#ifndef IPC_CONNECTION_H
#define IPC_CONNECTION_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "ipc.h"

/** 单条 TCP 连接 */
typedef struct ipc_connection
{
    uint32_t remote_module_id; /**< 对端模块 ID */
    int fd;                    /**< socket 文件描述符 */
    ipc_costate_t state;       /**< 连接状态 */

    /* 接收缓冲区 */
    uint8_t recv_buf[IPC_RECV_BUF_SIZE]; /**< 接收缓冲区 */
    uint32_t recv_len;                   /**< 已接收数据长度 */

    /* 心跳 */
    time_t last_heartbeat_sent; /**< 上次发送心跳的时间 */
    time_t last_heartbeat_recv; /**< 上次收到心跳的时间 */

    /* 重连 */
    uint32_t reconnect_delay_ms; /**< 当前重连延迟 */
    time_t next_reconnect_time;  /**< 下次重连时间 */

    /* 是否为主动发起方 */
    int is_initiator; /**< 1=主动连接方，0=被接受方 */
} ipc_connection_t;

/**
 * @brief 创建连接对象
 * @param remote_module_id 对端模块 ID
 * @param is_initiator 是否为主动连接方
 * @return 新连接对象
 */
ipc_connection_t *ipc_connection_create(uint32_t remote_module_id, int is_initiator);

/**
 * @brief 销毁连接对象
 * @param conn 连接对象
 */
void ipc_connection_destroy(ipc_connection_t *conn);

/**
 * @brief 发起非阻塞 TCP 连接
 * @param conn 连接对象
 * @param host 目标地址
 * @param port 目标端口
 * @return 成功或正在连接返回 0，失败返回 -1
 */
int ipc_connection_initiate(ipc_connection_t *conn, const char *host, uint16_t port);

/**
 * @brief 关闭连接
 * @param conn 连接对象
 */
void ipc_connection_close(ipc_connection_t *conn);

/**
 * @brief 发送完整帧数据（阻塞写入）
 * @param conn 连接对象
 * @param data 数据
 * @param len 数据长度
 * @return 成功返回 0，失败返回 -1
 */
int ipc_connection_send(ipc_connection_t *conn, const uint8_t *data, uint32_t len);

/**
 * @brief 重置重连延迟
 * @param conn 连接对象
 */
void ipc_connection_reset_reconnect(ipc_connection_t *conn);

/**
 * @brief 增加重连延迟（指数退避）
 * @param conn 连接对象
 */
void ipc_connection_backoff_reconnect(ipc_connection_t *conn);

#endif // IPC_CONNECTION_H
