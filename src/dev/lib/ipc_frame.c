/**
 * @file   ipc_frame.c
 * @brief  IPC 消息帧序列化/反序列化实现
 * @author jhb
 * @date   2026/02/02
 */

#include "ipc_frame.h"

#include <arpa/inet.h>
#include <glib.h>
#include <string.h>

#include "errcode.h"

int ipc_frame_serialize(const ipc_message_t *msg, uint8_t **out_buf, uint32_t *out_len)
{
    if (!msg || !out_buf || !out_len)
    {
        return ERRCODE_FAIL;
    }

    uint32_t payload_len = (uint32_t)msg->payload_len;
    uint32_t total_len = IPC_FRAME_HEADER_SIZE + payload_len;

    uint8_t *buf = g_malloc(total_len);
    uint8_t *p = buf;

    /* 写入帧头部（网络字节序） */
    uint32_t magic_be = htonl(IPC_MAGIC);
    memcpy(p, &magic_be, 4);
    p += 4;

    uint32_t msg_type_be = htonl(msg->msg_type);
    memcpy(p, &msg_type_be, 4);
    p += 4;

    uint32_t sender_be = htonl(msg->src_module_id);
    memcpy(p, &sender_be, 4);
    p += 4;

    uint32_t dst_be = htonl(msg->dst_module_id);
    memcpy(p, &dst_be, 4);
    p += 4;

    uint32_t req_be = htonl(msg->request_id);
    memcpy(p, &req_be, 4);
    p += 4;

    uint32_t plen_be = htonl(payload_len);
    memcpy(p, &plen_be, 4);
    p += 4;

    /* 写入负载 */
    if (msg->payload && payload_len > 0)
    {
        memcpy(p, msg->payload, payload_len);
    }

    *out_buf = buf;
    *out_len = total_len;
    return ERRCODE_SUCCESS;
}

int ipc_frame_parse_header(const uint8_t *buf, ipc_message_t *header)
{
    if (!buf || !header)
    {
        return ERRCODE_FAIL;
    }

    const uint8_t *p = buf;

    uint32_t val;
    memcpy(&val, p, 4);
    header->magic = ntohl(val);
    p += 4;

    memcpy(&val, p, 4);
    header->msg_type = ntohl(val);
    p += 4;

    memcpy(&val, p, 4);
    header->src_module_id = ntohl(val);
    p += 4;

    memcpy(&val, p, 4);
    header->dst_module_id = ntohl(val);
    p += 4;

    memcpy(&val, p, 4);
    header->request_id = ntohl(val);
    p += 4;

    memcpy(&val, p, 4);
    header->payload_len = ntohl(val);

    /* 校验魔数 */
    if (header->magic != IPC_MAGIC)
    {
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

ipc_message_t *ipc_frame_to_message(const ipc_message_t *header, const uint8_t *payload)
{
    if (!header)
    {
        return NULL;
    }

    void *data_copy = NULL;
    if (payload && header->payload_len > 0)
    {
        data_copy = g_memdup2(payload, header->payload_len);
    }

    return ipc_message_create(header->msg_type, header->src_module_id, header->dst_module_id, header->request_id,
                              data_copy, header->payload_len, g_free);
}
