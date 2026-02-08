/**
 * @file   telnet_access.c
 * @brief  Telnet接入模块实现
 * @author jhb
 * @date   2026/02/07
 */
#include "telnet_access.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <libgen.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"

#define TELNET_DEFAULT_PORT 3788
#define TELNET_BACKLOG 5
#define TELNET_MAX_EPOLL_EVENTS 16

// Telnet模块本地状态
typedef struct telnet_local {
    int epoll_fd;                // epoll文件描述符
    int listen_sock;             // 监听socket
    pthread_t worker_thread;     // 工作线程
    GHashTable *sessions;        // fd -> cli_session_t*
    volatile int running;        // 运行标志
    cli_engine_context_t *cli_ctx;  // CLI引擎上下文
} telnet_local_t;

static telnet_local_t *g_telnet_local = NULL;
static cli_engine_context_t *g_default_cli_ctx = NULL;

// 工作线程函数
static void *telnet_worker_thread(void *arg)
{
    (void)arg;

    struct sockaddr_in client_addr;
    socklen_t client_len;

    while (g_telnet_local && g_telnet_local->running)
    {
        struct epoll_event events[TELNET_MAX_EPOLL_EVENTS];
        int nfds = epoll_wait(g_telnet_local->epoll_fd, events, TELNET_MAX_EPOLL_EVENTS, 1000);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("[telnet] epoll_wait failed");
            break;
        }

        if (nfds == 0)
        {
            // Timeout
            continue;
        }

        // 处理事件
        for (int i = 0; i < nfds; i++)
        {
            if (events[i].data.fd == g_telnet_local->listen_sock)
            {
                // 新连接
                client_len = sizeof(client_addr);
                int client_fd = accept(g_telnet_local->listen_sock, 
                                      (struct sockaddr *)&client_addr, 
                                      &client_len);

                if (client_fd < 0)
                {
                    if (g_telnet_local && g_telnet_local->running)
                    {
                        perror("[telnet] Accept failed");
                    }
                    continue;
                }

                // 创建CLI会话
                cli_session_t *session = cli_engine_create_session(g_telnet_local->cli_ctx, client_fd);
                if (session)
                {
                    int *fd_key = g_malloc(sizeof(int));
                    *fd_key = client_fd;
                    g_hash_table_insert(g_telnet_local->sessions, fd_key, session);

                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = client_fd;
                    if (epoll_ctl(g_telnet_local->epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0)
                    {
                        perror("[telnet] Failed to add client to epoll");
                        g_hash_table_remove(g_telnet_local->sessions, fd_key);
                    }
                    else
                    {
                        printf("[telnet] Client connected (fd: %d)\n", client_fd);
                    }
                }
                else
                {
                    close(client_fd);
                }
            }
            else
            {
                // 现有客户端的输入
                int fd = events[i].data.fd;
                cli_session_t *session = g_hash_table_lookup(g_telnet_local->sessions, &fd);
                if (session)
                {
                    if (cli_engine_process_input(session) < 0)
                    {
                        printf("[telnet] Client disconnected (fd: %d)\n", fd);
                        epoll_ctl(g_telnet_local->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        g_hash_table_remove(g_telnet_local->sessions, &fd);
                    }
                }
            }
        }
    }

    return NULL;
}

// 创建监听socket
static int telnet_create_listen_sock(int port)
{
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("[telnet] Failed to create socket");
        return DEV_INVALID_FD;
    }

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        close(server_socket);
        perror("[telnet] Failed to set socket options");
        return DEV_INVALID_FD;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        close(server_socket);
        perror("[telnet] Failed to bind socket");
        return DEV_INVALID_FD;
    }

    if (listen(server_socket, TELNET_BACKLOG) < 0)
    {
        close(server_socket);
        perror("[telnet] Failed to listen");
        return DEV_INVALID_FD;
    }

    return server_socket;
}

// 接入模块操作接口实现
static int telnet_init(void *config)
{
    (void)config;

    if (g_telnet_local != NULL)
    {
        fprintf(stderr, "[telnet] Already initialized\n");
        return -1;
    }

    g_telnet_local = g_malloc0(sizeof(telnet_local_t));
    if (!g_telnet_local)
    {
        fprintf(stderr, "[telnet] Failed to allocate memory\n");
        return -1;
    }

    g_telnet_local->epoll_fd = DEV_INVALID_FD;
    g_telnet_local->listen_sock = DEV_INVALID_FD;
    g_telnet_local->worker_thread = 0;
    g_telnet_local->running = 0;
    g_telnet_local->sessions = g_hash_table_new_full(
        g_int_hash, 
        g_int_equal, 
        g_free, 
        (GDestroyNotify)cli_engine_destroy_session
    );

    // 初始化 CLI 引擎
    if (g_default_cli_ctx)
    {
        g_telnet_local->cli_ctx = g_default_cli_ctx;
    }
    else
    {
        // 兼容旧模式或独立测试
        g_telnet_local->cli_ctx = cli_engine_init(DEV_MODULE_ID_CFG, "access");
        if (!g_telnet_local->cli_ctx)
        {
            fprintf(stderr, "[telnet] Failed to initialize CLI engine\n");
            g_hash_table_destroy(g_telnet_local->sessions);
            g_free(g_telnet_local);
            g_telnet_local = NULL;
            return -1;
        }
    }

    return 0;
}

static int telnet_start(void)
{
    if (!g_telnet_local)
    {
        fprintf(stderr, "[telnet] Not initialized\n");
        return -1;
    }

    // 创建epoll
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        perror("[telnet] Failed to create epoll");
        return -1;
    }
    g_telnet_local->epoll_fd = epoll_fd;

    // 创建监听socket
    int listen_sock = telnet_create_listen_sock(TELNET_DEFAULT_PORT);
    if (listen_sock < 0)
    {
        return -1;
    }
    g_telnet_local->listen_sock = listen_sock;

    // 添加监听socket到epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_sock, &ev) < 0)
    {
        perror("[telnet] Failed to add listen socket to epoll");
        return -1;
    }

    g_telnet_local->running = 1;

    // 启动工作线程
    if (pthread_create(&g_telnet_local->worker_thread, NULL, telnet_worker_thread, NULL) != 0)
    {
        perror("[telnet] Failed to create worker thread");
        g_telnet_local->running = 0;
        return -1;
    }

    printf("[telnet] Server listening on port %d\n", TELNET_DEFAULT_PORT);
    return 0;
}

static int telnet_stop(void)
{
    if (!g_telnet_local)
    {
        return 0;
    }

    g_telnet_local->running = 0;

    if (g_telnet_local->listen_sock != DEV_INVALID_FD)
    {
        close(g_telnet_local->listen_sock);
        g_telnet_local->listen_sock = DEV_INVALID_FD;
    }

    if (g_telnet_local->worker_thread != 0)
    {
        pthread_join(g_telnet_local->worker_thread, NULL);
        g_telnet_local->worker_thread = 0;
    }

    printf("[telnet] Server stopped\n");
    return 0;
}

static void telnet_cleanup(void)
{
    if (!g_telnet_local)
    {
        return;
    }

    if (g_telnet_local->epoll_fd != DEV_INVALID_FD)
    {
        close(g_telnet_local->epoll_fd);
    }

    if (g_telnet_local->sessions)
    {
        g_hash_table_destroy(g_telnet_local->sessions);
    }

    g_free(g_telnet_local);
    g_telnet_local = NULL;

    printf("[telnet] Cleanup complete\n");
}

// 设置CLI引擎上下文（由cfg模块调用）
void telnet_set_cli_context(cli_engine_context_t *ctx)
{
    g_default_cli_ctx = ctx;
    if (g_telnet_local)
    {
        g_telnet_local->cli_ctx = ctx;
    }
}

// 导出的操作接口
access_module_ops_t telnet_access_ops = {
    .name = "telnet",
    .init = telnet_init,
    .start = telnet_start,
    .stop = telnet_stop,
    .cleanup = telnet_cleanup,
};
