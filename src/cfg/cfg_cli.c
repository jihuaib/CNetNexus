/**
 * @file   cfg_cli.c
 * @brief  CFG 模块 CLI 命令处理，处理 show、exit、config 等核心命令
 * @author jhb
 * @date   2026/01/22
 */

#include "cfg_cli.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cfg_main.h"
#include "cli_handler.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 命令树打印
// ============================================================================

static void print_commands_recursive(GString *out, const char *view_name, const char *prefix, cli_tree_node_t *node)
{
    if (!node)
    {
        return;
    }

    char new_prefix[MAX_CMD_LEN];
    if (strlen(prefix) > 0)
    {
        snprintf(new_prefix, sizeof(new_prefix), "%s %s", prefix, node->name ? node->name : "");
    }
    else
    {
        strncpy(new_prefix, node->name ? node->name : "", sizeof(new_prefix) - 1);
        new_prefix[sizeof(new_prefix) - 1] = '\0';
    }

    if (node->is_end_node == TRUE)
    {
        char module_name[DEV_MODULE_NAME_MAX_LEN];
        if (dev_get_module_name(g_cfg_local->dev_ipc_ctx, node->module_id, module_name) != ERRCODE_SUCCESS)
        {
            snprintf(module_name, sizeof(module_name), "unknown");
        }
        char buffer[2048];
        snprintf(buffer, sizeof(buffer), "  %-15s %-15s %s\r\n", view_name, module_name, new_prefix);
        g_string_append(out, buffer);
    }

    for (uint32_t i = 0; i < node->num_children; i++)
    {
        print_commands_recursive(out, view_name, new_prefix, node->children[i]);
    }
}

static void print_view_commands_flat(cli_view_node_t *view, GString *out)
{
    if (!view)
    {
        return;
    }

    if (view->cmd_tree)
    {
        for (uint32_t i = 0; i < view->cmd_tree->num_children; i++)
        {
            print_commands_recursive(out, view->view_name, "", view->cmd_tree->children[i]);
        }
    }

    for (uint32_t i = 0; i < view->num_children; i++)
    {
        print_view_commands_flat(view->children[i], out);
    }
}

// ============================================================================
// 分批输出辅助
// ============================================================================

static void cfg_cli_chunk_output(GString *full, cfg_cli_resp_out_t *resp_out)
{
    uint32_t offset = resp_out->batch_offset;
    if (offset >= full->len)
    {
        resp_out->message[0] = '\0';
        resp_out->has_more = 0;
        return;
    }

    size_t remaining = full->len - offset;
    size_t chunk = remaining < (CLI_MAX_RESP_LEN - 1) ? remaining : (CLI_MAX_RESP_LEN - 1);
    memcpy(resp_out->message, full->str + offset, chunk);
    resp_out->message[chunk] = '\0';
    resp_out->batch_offset = offset + chunk;

    resp_out->has_more = (resp_out->batch_offset < full->len) ? 1 : 0;
}

/* 缓存 show 输出 */
static GString *g_cfg_show_cache = NULL;

/* 缓存 history 输出 */
static GString *g_cfg_history_cache = NULL;

// ============================================================================
// 按 group_id 分发的 handler
// ============================================================================

/**
 * @brief show cli command-info (group_id=1)
 */
static void handle_show_commands(cli_session_t *session)
{
    GString *full_output = g_string_new("");

    cfg_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));

    /* 生成完整输出到缓存 */
    if (g_cfg_show_cache)
    {
        g_string_free(g_cfg_show_cache, TRUE);
    }
    g_cfg_show_cache = g_string_new("");

    g_string_append(g_cfg_show_cache, "\r\nCLI Commands List:\r\n");
    g_string_append(g_cfg_show_cache, "===================\r\n");
    g_string_append(g_cfg_show_cache, "  VIEW            MODULE          COMMAND\r\n");
    g_string_append(g_cfg_show_cache, "  ----            ------          -------\r\n");

    if (g_cfg_local->view_tree.root)
    {
        print_view_commands_flat(g_cfg_local->view_tree.root, g_cfg_show_cache);
    }

    g_string_append(g_cfg_show_cache, "\r\n");

    /* 分批读取 */
    do
    {
        resp_out.message[0] = '\0';
        resp_out.has_more = 0;
        cfg_cli_chunk_output(g_cfg_show_cache, &resp_out);
        if (resp_out.message[0] != '\0')
        {
            g_string_append(full_output, resp_out.message);
        }
    } while (resp_out.has_more);

    g_string_free(g_cfg_show_cache, TRUE);
    g_cfg_show_cache = NULL;

    if (full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }
    g_string_free(full_output, TRUE);
}

/**
 * @brief show cli history (group_id=2)
 */
static void handle_show_history(cli_session_t *session)
{
    GString *full_output = g_string_new("");
    cfg_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));

    if (g_cfg_history_cache)
    {
        g_string_free(g_cfg_history_cache, TRUE);
    }
    g_cfg_history_cache = g_string_new("");
    char buffer[512];

    pthread_mutex_lock(&g_cfg_local->history_mutex);

    g_string_append(g_cfg_history_cache, "\r\n");
    g_string_append(g_cfg_history_cache, "Command History:\r\n");
    g_string_append(g_cfg_history_cache,
                    "================================================================================\r\n");
    g_string_append(g_cfg_history_cache, " No  Time                Command                          Client IP\r\n");
    g_string_append(g_cfg_history_cache,
                    "--------------------------------------------------------------------------------\r\n");

    for (uint32_t i = 0; i < g_cfg_local->global_history.count; i++)
    {
        const cli_history_entry_t *entry =
            cli_global_history_get_entry(&g_cfg_local->global_history, g_cfg_local->global_history.count - 1 - i);
        if (entry && entry->command)
        {
            struct tm *timeinfo = localtime(&entry->timestamp);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);

            char cmd_display[33];
            if (strlen(entry->command) > 32)
            {
                strncpy(cmd_display, entry->command, 29);
                cmd_display[29] = '.';
                cmd_display[30] = '.';
                cmd_display[31] = '.';
                cmd_display[32] = '\0';
            }
            else
            {
                strcpy(cmd_display, entry->command);
            }

            snprintf(buffer, sizeof(buffer), " %-3u %-19s %-32s %-15s\r\n", i + 1, time_str, cmd_display,
                     entry->client_ip);
            g_string_append(g_cfg_history_cache, buffer);
        }
    }

    g_string_append(g_cfg_history_cache,
                    "================================================================================\r\n");
    snprintf(buffer, sizeof(buffer), "Total: %u command(s)\r\n\r\n", g_cfg_local->global_history.count);
    g_string_append(g_cfg_history_cache, buffer);

    pthread_mutex_unlock(&g_cfg_local->history_mutex);

    do
    {
        resp_out.message[0] = '\0';
        resp_out.has_more = 0;
        cfg_cli_chunk_output(g_cfg_history_cache, &resp_out);
        if (resp_out.message[0] != '\0')
        {
            g_string_append(full_output, resp_out.message);
        }
    } while (resp_out.has_more);

    g_string_free(g_cfg_history_cache, TRUE);
    g_cfg_history_cache = NULL;

    if (full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }
    g_string_free(full_output, TRUE);
}

/**
 * @brief show current-configuration (group_id=3)
 *
 * 向所有业务模块发送 CFG_MSG_TYPE_SHOW_CONFIG，收集响应并聚合输出。
 * 未连接的模块直接跳过，避免超时等待。
 */
static void handle_show_config(cli_session_t *session)
{
    /* 需要查询配置的业务模块列表 */
    static const uint32_t config_modules[] = {DEV_MODULE_ID_BGP};

    GString *output = g_string_new("");

    for (size_t i = 0; i < G_N_ELEMENTS(config_modules); i++)
    {
        uint32_t mod_id = config_modules[i];
        if (!dev_ipc_is_connected(g_cfg_local->dev_ipc_ctx, mod_id))
        {
            continue;
        }

        dev_ipc_message_t *req =
            dev_ipc_message_create(CFG_MSG_TYPE_SHOW_CONFIG, DEV_MODULE_ID_CFG, mod_id, 0, NULL, 0, NULL);
        if (!req)
        {
            continue;
        }

        dev_ipc_message_t *resp = dev_ipc_query(g_cfg_local->dev_ipc_ctx, mod_id, req, 2000);
        dev_ipc_message_free(req);

        if (resp)
        {
            /* payload 为 NULL 结尾的字符串，payload_len > 1 才有实际内容 */
            if (resp->payload && resp->payload_len > 1)
            {
                g_string_append(output, (char *)resp->payload);
            }
            dev_ipc_message_free(resp);
        }
    }

    if (output->len > 0)
    {
        cli_pager_output(session, output->str);
    }
    else
    {
        cfg_send_message(session, "No configuration found.\r\n");
    }

    g_string_free(output, TRUE);
}

/**
 * @brief exit (group_id=4)
 */
static void handle_op_exit(cli_session_t *session)
{
    cli_view_node_t *parent_view = session->current_view->parent;
    if (parent_view == NULL)
    {
        close(session->client_fd);
    }
    else
    {
        cli_view_node_t *config_view = cli_view_find_by_id(g_cfg_local->view_tree.root, parent_view->view_id);
        if (config_view)
        {
            session->current_view = config_view;
            cli_prompt_pop(session);
        }
    }
}

/**
 * @brief config (group_id=5)
 */
static void handle_op_config(cli_session_t *session)
{
    cli_view_node_t *config_view = cli_view_find_by_id(g_cfg_local->view_tree.root, CLI_VIEW_CONFIG);
    if (config_view)
    {
        cli_prompt_push(session);
        session->current_view = config_view;
        update_prompt_from_template(session, session->current_view->prompt_template);
    }
}

// ============================================================================
// Bash 模式：转发 telnet 会话与 PTY 之间的数据
// ============================================================================

/**
 * @brief bash 桥接线程上下文
 */
typedef struct
{
    cli_session_t *session; /**< 当前 CLI 会话 */
    int client_fd;          /**< 客户端 socket fd */
} bash_bridge_ctx_t;

/**
 * @brief bash 桥接线程：在客户端 socket 与 bash PTY 之间双向转发数据
 */
static void *bash_bridge_thread(void *arg)
{
    bash_bridge_ctx_t *ctx = (bash_bridge_ctx_t *)arg;
    cli_session_t *session = ctx->session;
    int client_fd = ctx->client_fd;
    g_free(ctx);

    /* 创建 PTY 并 fork bash */
    int pty_master;
    struct winsize ws = {.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
    pid_t pid = forkpty(&pty_master, NULL, NULL, &ws);

    if (pid < 0)
    {
        /* fork 失败，恢复 epoll 监听 */
        cfg_send_message(session, "Error: Failed to start bash.\r\n");
        session->bash_mode = 0;
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = client_fd;
        epoll_ctl(g_cfg_local->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
        send_prompt(session);
        return NULL;
    }

    if (pid == 0)
    {
        /* 子进程：执行 bash */
        setenv("TERM", "xterm", 1);
        execlp("/bin/bash", "bash", "--login", NULL);
        _exit(1);
    }

    /* 父进程：双向数据转发 */
    char buf[4096];
    struct pollfd fds[2];

    while (1)
    {
        fds[0].fd = client_fd;
        fds[0].events = POLLIN;
        fds[1].fd = pty_master;
        fds[1].events = POLLIN;

        int ret = poll(fds, 2, 500);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        /* 检查 bash 是否已退出 */
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid)
        {
            pid = -1;
            break;
        }

        if (fds[0].revents & POLLIN)
        {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0)
            {
                /* 客户端断开 */
                break;
            }
            if (write(pty_master, buf, (size_t)n) < 0)
            {
                break;
            }
        }

        if (fds[1].revents & POLLIN)
        {
            ssize_t n = read(pty_master, buf, sizeof(buf));
            if (n <= 0)
            {
                /* bash 已退出或 PTY 关闭 */
                break;
            }
            if (write(client_fd, buf, (size_t)n) < 0)
            {
                break;
            }
        }
    }

    /* 清理：等待 bash 子进程退出 */
    if (pid > 0)
    {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
    close(pty_master);

    /* 恢复 epoll 监听，返回 CLI */
    session->bash_mode = 0;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = client_fd;
    epoll_ctl(g_cfg_local->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

    cfg_send_message(session, "\r\nBash 会话结束，返回 CLI...\r\n");
    send_prompt(session);

    return NULL;
}

/**
 * @brief bash (group_id=7)：暂时将客户端 socket 移出 epoll，由后台线程桥接 bash PTY
 */
static void handle_op_bash(cli_session_t *session)
{
    /* 从 epoll 中移除，防止主线程继续读取该 socket */
    epoll_ctl(g_cfg_local->epoll_fd, EPOLL_CTL_DEL, session->client_fd, NULL);

    /* 标记会话进入 bash 模式（主循环不再发送 CLI 提示符） */
    session->bash_mode = 1;

    cfg_send_message(session, "\r\n进入 bash shell，输入 'exit' 返回 CLI。\r\n\r\n");

    /* 创建桥接线程 */
    bash_bridge_ctx_t *ctx = g_malloc(sizeof(*ctx));
    ctx->session = session;
    ctx->client_fd = session->client_fd;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&tid, &attr, bash_bridge_thread, ctx) != 0)
    {
        /* 线程创建失败，立即恢复 */
        g_free(ctx);
        session->bash_mode = 0;
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = session->client_fd;
        epoll_ctl(g_cfg_local->epoll_fd, EPOLL_CTL_ADD, session->client_fd, &ev);
        cfg_send_message(session, "Error: Failed to create bash thread.\r\n");
    }

    pthread_attr_destroy(&attr);
}

/**
 * @brief end (group_id=6)
 */
static void handle_op_end(cli_session_t *session)
{
    cli_view_node_t *user_view = cli_view_find_by_id(g_cfg_local->view_tree.root, CLI_VIEW_USER);
    if (user_view)
    {
        session->current_view = user_view;
        /* 释放所有上下文栈数据并重置 prompt 栈 */
        for (uint32_t i = 0; i < session->prompt_stack_depth; i++)
        {
            if (session->view_context_stack[i])
            {
                g_free(session->view_context_stack[i]);
                session->view_context_stack[i] = NULL;
                session->view_context_len[i] = 0;
            }
        }
        session->prompt_stack_depth = 0;
        update_prompt_from_template(session, session->current_view->prompt_template);
    }
}

// ============================================================================
// 主入口
// ============================================================================

int cfg_cli_handle(dev_ipc_message_t *msg, cli_session_t *session)
{
    if (!msg || !msg->payload || !session)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到命令 (group_id=%u)", parser.group_id);

    switch (parser.group_id)
    {
        case CFG_CLI_GROUP_ID_SHOW_COMMANDS:
            handle_show_commands(session);
            break;
        case CFG_CLI_GROUP_ID_SHOW_HISTORY:
            handle_show_history(session);
            break;
        case CFG_CLI_GROUP_ID_SHOW_CONFIG:
            handle_show_config(session);
            break;
        case CFG_CLI_GROUP_ID_EXIT:
            handle_op_exit(session);
            break;
        case CFG_CLI_GROUP_ID_CONFIG:
            handle_op_config(session);
            break;
        case CFG_CLI_GROUP_ID_END:
            handle_op_end(session);
            break;
        case CFG_CLI_GROUP_ID_BASH:
            handle_op_bash(session);
            break;
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            cfg_send_message(session, "Error: Unknown CFG command.\r\n");
            cli_tlv_cleanup(&parser);
            return ERRCODE_FAIL;
    }

    cli_tlv_cleanup(&parser);
    return ERRCODE_SUCCESS;
}
