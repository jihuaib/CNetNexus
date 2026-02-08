/**
 * @file   cli_engine.c
 * @brief  CLI引擎实现 - 会话管理和命令处理
 * @author jhb
 * @date   2026/02/07
 */
#include "cli_engine.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_handler.h"
#include "errcode.h"

// 全局CLI引擎上下文（内部使用）
// 全局CLI引擎上下文
cli_engine_context_t *g_cli_engine_ctx = NULL;

static void cli_ipc_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    (void)ctx;
    if (msg)
    {
        // CLI 引擎通常作为客户端发起请求，不处理 unsolicited 消息
        // 除非有特定的通知机制
        ipc_message_free(msg);
    }
}

cli_engine_context_t *cli_engine_init(uint32_t module_id, const char *module_name)
{
    if (g_cli_engine_ctx != NULL)
    {
        fprintf(stderr, "[cli_engine] Already initialized\n");
        return g_cli_engine_ctx;
    }

    cli_engine_context_t *ctx = g_malloc0(sizeof(cli_engine_context_t));
    if (!ctx)
    {
        fprintf(stderr, "[cli_engine] Failed to allocate context\n");
        return NULL;
    }

    // 初始化视图树为空（由调用者加载XML）
    ctx->view_tree.root = NULL;
    ctx->view_tree.root = NULL;
    ctx->user_data = NULL;

    // 初始化 IPC
    ctx->ipc_ctx = ipc_init(module_id, module_name, NULL, cli_ipc_msg_handler);
    if (!ctx->ipc_ctx)
    {
        fprintf(stderr, "[cli_engine] Failed to initialize IPC\n");
        g_free(ctx);
        return NULL;
    }

    g_cli_engine_ctx = ctx;

    printf("[cli_engine] Initialized (module_id=0x%08X, name=%s)\n", module_id, module_name);
    return ctx;
}

void cli_engine_cleanup(cli_engine_context_t *ctx)
{
    if (!ctx)
    {
        return;
    }

    // 销毁 IPC 上下文
    if (ctx->ipc_ctx)
    {
        ipc_destroy(ctx->ipc_ctx);
        ctx->ipc_ctx = NULL;
    }

    if (ctx == g_cli_engine_ctx)
    {
        g_cli_engine_ctx = NULL;
    }

    g_free(ctx);
    printf("[cli_engine] Cleanup complete\n");
}

cli_view_tree_t *cli_engine_get_view_tree(cli_engine_context_t *ctx)
{
    return ctx ? &ctx->view_tree : NULL;
}

ipc_context_t *cli_engine_get_ipc_context(cli_engine_context_t *ctx)
{
    return ctx ? ctx->ipc_ctx : NULL;
}

void cli_engine_set_user_data(cli_engine_context_t *ctx, void *user_data)
{
    if (ctx)
    {
        ctx->user_data = user_data;
    }
}

void *cli_engine_get_user_data(cli_engine_context_t *ctx)
{
    return ctx ? ctx->user_data : NULL;
}

cli_session_t *cli_engine_create_session(cli_engine_context_t *ctx, int client_fd)
{
    if (!ctx)
    {
        fprintf(stderr, "[cli_engine] Invalid context\n");
        return NULL;
    }

    cli_session_t *session = cli_session_create(client_fd);
    if (!session)
    {
        return NULL;
    }

    // 设置session的视图为根视图
    session->current_view = ctx->view_tree.root;

    return session;
}

void cli_engine_destroy_session(cli_session_t *session)
{
    if (session)
    {
        cli_session_destroy(session);
    }
}

int cli_engine_process_input(cli_session_t *session)
{
    return cli_process_input(session);
}
