/**
 * @file   vrf_main.c
 * @brief  VRF 模块主文件：IPC 初始化、三阶段生命周期、VRF 表管理及 RPC 处理
 * @author jhb
 * @date   2026/03/05
 */
#include "vrf_main.h"

#include <pthread.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "vrf_cli.h"

/* 全局状态 */
vrf_local_t *g_vrf_local = NULL;

// ============================================================================
// VRF 表操作
// ============================================================================

vrf_entry_t *vrf_create(const char *name)
{
    if (!name || name[0] == '\0')
    {
        return NULL;
    }

    /* 检查名称是否已存在 */
    if (g_hash_table_lookup(g_vrf_local->vrf_by_name, name))
    {
        return NULL;
    }

    vrf_entry_t *entry = g_new0(vrf_entry_t, 1);
    entry->vrf_id = g_vrf_local->next_id++;
    snprintf(entry->name, sizeof(entry->name), "%s", name);

    g_hash_table_insert(g_vrf_local->vrf_by_id, GUINT_TO_POINTER(entry->vrf_id), entry);
    g_hash_table_insert(g_vrf_local->vrf_by_name, entry->name, entry);

    LOG_INFO("VRF created: id=%u name=%s", entry->vrf_id, entry->name);
    return entry;
}

vrf_entry_t *vrf_find_by_id(uint32_t vrf_id)
{
    return g_hash_table_lookup(g_vrf_local->vrf_by_id, GUINT_TO_POINTER(vrf_id));
}

vrf_entry_t *vrf_find_by_name(const char *name)
{
    if (!name)
    {
        return NULL;
    }
    return g_hash_table_lookup(g_vrf_local->vrf_by_name, name);
}

int vrf_delete(const char *name)
{
    vrf_entry_t *entry = vrf_find_by_name(name);
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    uint32_t vrf_id = entry->vrf_id;
    g_hash_table_remove(g_vrf_local->vrf_by_name, entry->name);
    g_hash_table_remove(g_vrf_local->vrf_by_id, GUINT_TO_POINTER(vrf_id));
    g_free(entry);

    LOG_INFO("VRF deleted: id=%u name=%s", vrf_id, name);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 三阶段辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_VRF,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START
// ============================================================================

static void vrf_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");
    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT
// ============================================================================

static void vrf_on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY - Creating public VRF
// ============================================================================

static void vrf_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Creating public VRF");

    /* 手动插入公网 VRF（ID=0，跳过 next_id 分配） */
    vrf_entry_t *pub = g_new0(vrf_entry_t, 1);
    pub->vrf_id = VRF_PUBLIC_VRF_ID;
    snprintf(pub->name, sizeof(pub->name), "%s", VRF_PUBLIC_VRF_NAME);
    g_hash_table_insert(g_vrf_local->vrf_by_id, GUINT_TO_POINTER(pub->vrf_id), pub);
    g_hash_table_insert(g_vrf_local->vrf_by_name, pub->name, pub);
    LOG_INFO("Public VRF created: id=%u name=%s", pub->vrf_id, pub->name);

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// VRF RPC 处理：VRF_MSG_TYPE_GET_NAME
// ============================================================================

static void vrf_handle_get_name(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    const char *name = NULL;

    if (msg->payload && msg->payload_len >= sizeof(uint32_t))
    {
        uint32_t vrf_id;
        memcpy(&vrf_id, msg->payload, sizeof(uint32_t));
        vrf_entry_t *entry = vrf_find_by_id(vrf_id);
        if (entry)
        {
            name = entry->name;
        }
    }

    char *resp_payload = name ? g_strdup(name) : NULL;
    uint32_t resp_len = name ? (uint32_t)(strlen(name) + 1) : 0;

    dev_ipc_message_t *resp =
        dev_ipc_message_create(VRF_MSG_TYPE_RESP, DEV_MODULE_ID_VRF, msg->src_module_id, msg->request_id, resp_payload,
                               resp_len, resp_payload ? g_free : NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
    dev_ipc_message_free(msg);
}

// ============================================================================
// IPC 消息路由
// ============================================================================

void vrf_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            vrf_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            vrf_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            vrf_on_ready(msg);
            return;
        case VRF_MSG_TYPE_GET_NAME:
            vrf_handle_get_name(msg);
            return;
        case CLI_MSG_TYPE_QUERY_CANDIDATES:
            vrf_cli_handle_query_candidates(msg);
            return;
        case CLI_MSG_TYPE:
            vrf_cli_handle_message(msg);
            break;
        case CLI_MSG_TYPE_CONTINUE:
            vrf_cli_handle_continue(msg);
            break;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            vrf_cli_handle_show_config(msg);
            break;
        default:
            LOG_WARN("Unknown message type: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// Module initialization
// ============================================================================

int vrf_module_init(void)
{
    log_set_tag("vrf");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_VRF, "vrf", DEV_MODULE_PORT_VRF, vrf_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_vrf_local = g_new0(vrf_local_t, 1);
    g_vrf_local->dev_ipc_ctx = ctx;
    g_vrf_local->vrf_by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_vrf_local->vrf_by_name = g_hash_table_new(g_str_hash, g_str_equal);
    g_vrf_local->next_id = 1; /* 0 保留给公网 VRF */
    return 0;
}

void vrf_module_cleanup(void)
{
    if (!g_vrf_local)
    {
        return;
    }

    dev_ipc_context_t *ctx = g_vrf_local->dev_ipc_ctx;
    g_vrf_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    if (!g_vrf_local)
    {
        return;
    }

    vrf_cli_cleanup_state();

    if (g_vrf_local->vrf_by_id)
    {
        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init(&iter, g_vrf_local->vrf_by_id);
        while (g_hash_table_iter_next(&iter, &key, &val))
        {
            g_free(val);
        }
        g_hash_table_destroy(g_vrf_local->vrf_by_id);
        g_vrf_local->vrf_by_id = NULL;
    }

    if (g_vrf_local->vrf_by_name)
    {
        g_hash_table_destroy(g_vrf_local->vrf_by_name);
        g_vrf_local->vrf_by_name = NULL;
    }

    g_free(g_vrf_local);
    g_vrf_local = NULL;
}
