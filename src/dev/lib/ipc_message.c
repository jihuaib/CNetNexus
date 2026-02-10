/**
 * @file   ipc_message.c
 * @brief  IPC 消息管理：消息创建/释放、模块名称查询
 * @author jhb
 * @date   2026/02/06
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "dev.h"
#include "errcode.h"
#include "ipc.h"

// ============================================================================
// 消息管理
// ============================================================================

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

// ============================================================================
// 模块名称查询（静态表）
// ============================================================================

/** 模块 ID 到名称的静态映射表 */
static const struct
{
    uint32_t id;
    const char *name;
} g_module_names[] = {
    {DEV_MODULE_ID_DEV, "dev"}, {DEV_MODULE_ID_DB, "db"},   {DEV_MODULE_ID_CFG, "cfg"},
    {DEV_MODULE_ID_IF, "if"},   {DEV_MODULE_ID_BGP, "bgp"},
};

const char *ipc_get_module_name(uint32_t module_id)
{
    for (size_t i = 0; i < sizeof(g_module_names) / sizeof(g_module_names[0]); i++)
    {
        if (g_module_names[i].id == module_id)
        {
            return g_module_names[i].name;
        }
    }

    return "unknown";
}

int dev_get_module_name(uint32_t module_id, char *module_name)
{
    if (!module_name)
    {
        return -1;
    }

    const char *name = ipc_get_module_name(module_id);
    if (strcmp(name, "unknown") == 0)
    {
        return -1;
    }

    strncpy(module_name, name, DEV_MODULE_NAME_MAX_LEN - 1);
    module_name[DEV_MODULE_NAME_MAX_LEN - 1] = '\0';
    return 0;
}
