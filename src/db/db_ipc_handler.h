/**
 * @file   db_ipc_handler.h
 * @brief  数据库模块 IPC 消息处理器头文件
 * @author jhb
 * @date   2026/02/02
 *
 * 在 DB 进程端接收来自其他模块的 DB RPC 消息，
 * 调用本地 db_api.c 执行操作后返回结果。
 */

#ifndef DB_IPC_HANDLER_H
#define DB_IPC_HANDLER_H

#include "dev.h"

/**
 * @brief DB 模块的 IPC 消息处理回调
 *
 * 处理 DEV_IPC_MSG_TYPE_DB_INSERT/UPDATE/DELETE/QUERY/EXISTS 消息，
 * 反序列化参数、执行本地 DB 操作、序列化结果并发送响应。
 *
 * @param msg 接收到的消息
 */
void db_ipc_msg_handler(dev_ipc_message_t *msg);

#endif // DB_IPC_HANDLER_H
