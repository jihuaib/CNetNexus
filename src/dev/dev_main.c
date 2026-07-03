/**
 * @file   dev_main.c
 * @brief  Dev 模块主入口，模块注册和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */

#include "dev_main.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "dev_bdr.h"
#include "dev_cli.h"
#include "dev_module.h"
#include "dev_subscribe.h"
#include "errcode.h"
#include "log.h"

dev_local_t *g_dev_local = NULL;

// ============================================================================
// IPC 消息处理回调
// ============================================================================

/** 处理模块名称查询请求 */
static void handle_dev_get_module_name(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    char name[DEV_MODULE_NAME_MAX_LEN] = "";

    if (msg->payload && msg->payload_len >= sizeof(uint32_t))
    {
        uint32_t module_id;
        memcpy(&module_id, msg->payload, sizeof(uint32_t));
        dev_get_module_name_inner(module_id, name);
    }

    /* 响应 payload 为模块名称字符串（含终止符） */
    size_t name_len = strlen(name) + 1;
    char *resp_data = g_strdup(name);
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_DEV,
                                                     msg->src_module_id, msg->request_id, resp_data, name_len, g_free);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
}

/** 处理模块 PRE_EXIT 通知：在模块真正 exit() 前同步完成 DEV 侧清理 */
static void handle_dev_pre_exit(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    uint32_t module_id = msg->src_module_id;

    /* `reboot software` 路径下 cleanup_all_modules 已经在串行：SIGTERM、waitpid、
     * g_free(module)。此时 PRE_EXIT handler 若再去改 m->child_pid / 调 drop_connection
     * 会与主线程的释放路径 race（worker 触碰 g_free'd m → UAF；child_pid 被清零导致
     * cleanup_all_modules Step 2 跳过 waitpid → 子进程后续退出无人 reap）。
     * 这里只回 ACK，让模块继续 exit；后续清理交给 cleanup_all_modules + 框架的
     * IO 线程"Connection lost"路径。 */
    if (dev_module_is_cleanup_in_progress())
    {
        LOG_INFO("PRE_EXIT received from id=0x%08X during cleanup_all_modules; ACK only", module_id);
        dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_PRE_EXIT_RESP, DEV_MODULE_ID_DEV,
                                                         module_id, msg->request_id, NULL, 0, NULL);
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
        return;
    }

    dev_module_t *m = dev_module_find(module_id);

    if (m)
    {
        LOG_INFO("PRE_EXIT received from %s (id=0x%08X)", m->name, module_id);
        /* 1) 翻状态 + 标记 pre_cleaned + 把 child_pid 挪到 pre_cleaned_pid。
         *    把 child_pid 清零，让随后 SUBSCRIBE handler 的 "child_pid > 0" 短路分支
         *    不再误把这个即将退出的进程当成 STARTING 报给订阅者；老 pid 转存到
         *    pre_cleaned_pid，SIGCHLD 的 find_by_pid 仍能找到本模块。 */
        m->phase = DEV_PHASE_REGISTERED;
        m->pre_cleaned = 1;
        m->pre_cleaned_pid = m->child_pid;
        m->child_pid = 0;

        /* 2) 通知订阅者：模块下线 */
        dev_subscribe_broadcast_event(m, DEV_MODULE_EVENT_DOWN);
    }
    else
    {
        LOG_WARN("PRE_EXIT from unknown module id=0x%08X", module_id);
    }

    /* 3) ACK 先发出去（drop 之前），让对端收到 ACK 即可放心 exit。
     *    TCP close 会在 FIN 之前把 send buffer 里的 ACK bytes 刷出，对端先读到
     *    ACK，再读到 EOF —— 顺序天然正确。 */
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_PRE_EXIT_RESP, DEV_MODULE_ID_DEV, module_id,
                                                     msg->request_id, NULL, 0, NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);

    /* 4) 同步摘掉 IPC 连接记录。drop_connection 会 pthread_join IO 线程；
     *    此时对端进程尚未退出（仍在等我们 ACK），IO 线程会正常 epoll_wait 超时退出，
     *    pthread_join 一般几十 ms 返回。 */
    if (m && ctx)
    {
        dev_ipc_drop_connection(ctx, module_id);
    }
}

void dev_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_GET_MODULE_NAME:
            handle_dev_get_module_name(msg);
            break;

        case DEV_IPC_MSG_TYPE_DEV_SUBSCRIBE_MODULE:
            dev_subscribe_handle_subscribe(msg);
            break;

        case DEV_IPC_MSG_TYPE_DEV_UNSUBSCRIBE_MODULE:
            dev_subscribe_handle_unsubscribe(msg);
            break;

        case DEV_IPC_MSG_TYPE_DEV_NOTIFY_READY:
            dev_subscribe_handle_notify_ready(msg);
            break;

        case DEV_IPC_MSG_TYPE_DEV_PRE_EXIT:
            handle_dev_pre_exit(msg);
            break;

        case CLI_MSG_TYPE_QUERY_CANDIDATES:
            dev_cli_handle_query_candidates(msg);
            return; /* msg 由 handler 内部释放 */

        case CLI_MSG_TYPE:
            LOG_DEBUG("Received CLI command message");
            dev_cli_handle_message(msg);
            break;

        case CLI_MSG_TYPE_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            dev_cli_handle_continue(msg);
            break;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("Received show current-configuration request");
            dev_bdr_show_config(msg);
            break;

        case CLI_MSG_TYPE_LINE_CLOSED:
            dev_cli_handle_line_closed(msg);
            break;

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

/**
 * @brief DEV 自身初始化（在三阶段流程开始前调用）
 *
 * 创建 DEV 的 IPC context 并设置到全局上下文和模块注册表中。
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int dev_init_self(void)
{
    LOG_INFO("DEV IPC initialization started========================");
    g_dev_local = g_malloc0(sizeof(dev_local_t));

    g_dev_local->dev_ipc_ctx = dev_ipc_init(DEV_MODULE_ID_DEV, "dev", DEV_MODULE_PORT_DEV, dev_msg_handler);
    if (!g_dev_local->dev_ipc_ctx)
    {
        LOG_ERROR("Failed to initialize IPC");
        g_free(g_dev_local);
        g_dev_local = NULL;
        return ERRCODE_FAIL;
    }

    /* 注册 DEV 模块到 GTree，并补充端口号；DEV 自身的 READY 推迟到 dev_init_all_modules
     * 末尾再置：含义为 “supervisor 完成了基础模块拉起 / DB 恢复 / 按需模块 revive”。 */
    dev_module_t *dev_self = dev_add_module_to_registry(DEV_MODULE_ID_DEV, "dev");
    if (dev_self)
    {
        dev_self->port = DEV_MODULE_PORT_DEV;
        dev_self->phase = DEV_PHASE_LOADED;
    }

    LOG_INFO("DEV IPC initialization complete========================");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 广播 log-level 给所有已注册模块（DEV 自身已本地生效，无需再发自己）
 */
static gboolean broadcast_log_level_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    uint32_t level_be = *(uint32_t *)user_data;

    if (module->module_id == DEV_MODULE_ID_DEV || module->phase < DEV_PHASE_LOADED)
    {
        return FALSE;
    }

    dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_SET_LOG_LEVEL, DEV_MODULE_ID_DEV,
                                                    module->module_id, 0, &level_be, sizeof(level_be), NULL);
    if (!req)
    {
        return FALSE;
    }
    /* 单向通知：接收端在 IPC 库层透明处理，无需等待响应 */
    dev_ipc_send(g_dev_local->dev_ipc_ctx, module->module_id, req);
    dev_ipc_message_free(req);
    return FALSE;
}

void dev_broadcast_log_level(uint32_t level)
{
    uint32_t level_be = htonl(level);
    dev_module_foreach(broadcast_log_level_cb, &level_be);
}

static gboolean broadcast_syslog_remote_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    const syslog_report_remote_config_t *cfg = (const syslog_report_remote_config_t *)user_data;

    if (!cfg || module->module_id == DEV_MODULE_ID_DEV || module->phase < DEV_PHASE_LOADED)
    {
        return FALSE;
    }

    syslog_report_remote_config_t payload = *cfg;
    payload.enabled = htonl(payload.enabled);
    payload.port = htonl(payload.port);
    payload.server[sizeof(payload.server) - 1] = '\0';

    dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_SET_SYSLOG_REMOTE, DEV_MODULE_ID_DEV,
                                                    module->module_id, 0, &payload, sizeof(payload), NULL);
    if (!req)
    {
        return FALSE;
    }
    dev_ipc_send(g_dev_local->dev_ipc_ctx, module->module_id, req);
    dev_ipc_message_free(req);
    return FALSE;
}

void dev_broadcast_syslog_remote(const syslog_report_remote_config_t *cfg)
{
    if (!cfg)
    {
        return;
    }
    dev_module_foreach(broadcast_syslog_remote_cb, (void *)cfg);
}

/**
 * @brief 获取 DEV 的 IPC context
 * @return DEV 的 IPC context
 */
dev_ipc_context_t *dev_get_ipc_ctx(void)
{
    if (g_dev_local)
    {
        return g_dev_local->dev_ipc_ctx;
    }
    return NULL;
}

/**
 * @brief DEV 清理
 */
void dev_cleanup_self(void)
{
    if (g_dev_local == NULL)
    {
        return;
    }

    LOG_INFO("Dev module cleanup");

    dev_cli_cleanup_state();

    /* 注意：IPC context 的销毁由 cleanup_all_modules() 统一处理 */
    g_dev_local->dev_ipc_ctx = NULL;

    g_free(g_dev_local);
    g_dev_local = NULL;
}
