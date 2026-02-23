/**
 * @file   ipc_message.c
 * @brief  IPC 消息管理：消息创建与释放
 * @author jhb
 * @date   2026/02/06
 */
#include <glib.h>

#include "errcode.h"
#include "ipc.h"

ipc_message_t *ipc_message_create(uint32_t msg_type, uint32_t src_module_id, uint32_t dst_module_id,
                                  uint32_t request_id, void *payload, size_t payload_len, void (*free_fn)(void *))
{
    ipc_message_t *msg = g_malloc0(sizeof(ipc_message_t));

    msg->magic = IPC_MAGIC;
    msg->msg_type = msg_type;
    msg->src_module_id = src_module_id;
    msg->dst_module_id = dst_module_id;
    msg->request_id = request_id;
    msg->payload_len = (uint32_t)payload_len;
    msg->payload = payload;
    msg->free_fn = free_fn;

    return msg;
}

void ipc_message_free(ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    if (msg->payload && msg->free_fn)
    {
        msg->free_fn(msg->payload);
    }

    g_free(msg);
}
