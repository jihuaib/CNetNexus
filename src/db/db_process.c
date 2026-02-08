/**
 * @file   db_process.c
 * @brief  数据库模块独立进程入口
 * @author jhb
 * @date   2026/02/03
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "db_main.h"
#include "errcode.h"
#include "ipc.h"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("[db] DB 进程启动 (pid=%d)\n", getpid());

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sig_fd < 0)
    {
        perror("[db] signalfd 创建失败");
        return EXIT_FAILURE;
    }

    /* 初始化 DB 模块（创建连接、注册表、IPC 服务） */
    if (db_module_init() != ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[db] DB 模块初始化失败\n");
        close(sig_fd);
        return EXIT_FAILURE;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        perror("[db] epoll 创建失败");
        db_module_cleanup();
        close(sig_fd);
        return EXIT_FAILURE;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sig_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sig_fd, &ev);

    printf("[db] DB 进程就绪，等待请求...\n");

    while (g_db_local)
    {
        struct epoll_event events[1];
        int nfds = epoll_wait(epoll_fd, events, 1, 1000);

        if (nfds > 0 && events[0].data.fd == sig_fd)
        {
            struct signalfd_siginfo siginfo;
            read(sig_fd, &siginfo, sizeof(siginfo));
            printf("[db] 收到信号 %d，准备退出\n", siginfo.ssi_signo);
            break;
        }

        if (g_db_local && g_db_local->ipc_ctx &&
            ipc_shutdown_requested(g_db_local->ipc_ctx))
        {
            printf("[db] 收到 IPC 关闭请求\n");
            break;
        }
    }

    db_module_cleanup();
    close(epoll_fd);
    close(sig_fd);

    printf("[db] DB 进程退出\n");
    return EXIT_SUCCESS;
}
