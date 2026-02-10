/**
 * @file   ipc_frame.h
 * @brief  IPC 消息帧序列化/反序列化头文件
 * @author jhb
 * @date   2026/02/02
 */

#ifndef IPC_FRAME_H
#define IPC_FRAME_H

#include <stdint.h>

#include "dev.h"
#include "ipc.h"

/**
 * @brief 将 ipc_message_t 序列化为 IPC 帧（头部 + 负载）
 * @param msg 消息
 * @param out_buf 输出缓冲区指针（由函数分配，调用者 g_free）
 * @param out_len 输出总长度
 * @return 成功返回 0，失败返回 -1
 */
int ipc_frame_serialize(const ipc_message_t *msg, uint8_t **out_buf, uint32_t *out_len);

/**
 * @brief 从帧头部缓冲区解析头部
 * @param buf 24 字节帧头部
 * @param header 输出头部结构
 * @return 成功返回 0，失败返回 -1
 */
int ipc_frame_parse_header(const uint8_t *buf, ipc_message_t *header);

/**
 * @brief 从帧头部和负载重建 ipc_message_t
 * @param header 已解析的帧头部
 * @param payload 负载数据（函数会拷贝）
 * @return 新创建的消息，失败返回 NULL
 */
ipc_message_t *ipc_frame_to_message(const ipc_message_t *header, const uint8_t *payload);

#endif // IPC_FRAME_H
