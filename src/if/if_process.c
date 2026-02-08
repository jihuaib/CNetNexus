/**
 * @file   if_process.c
 * @brief  IF 独立进程入口
 * @author jhb
 * @date   2026/02/03
 *
 * IF 模块独立进程的 main() 函数，负责信号处理、
 * 模块初始化和主循环。
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "errcode.h"
#include "if_main.h"
#include "ipc.h"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("[if] IF 进程启动 (pid=%d)\n", getpid());

    /* 屏蔽 SIGINT/SIGTERM，使用 signalfd 处理 */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sig_fd < 0)
    {
        perror("[if] signalfd 创建失败");
        return EXIT_FAILURE;
    }

    /* 初始化 IF 模块 */
    if (if_init() != ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[if] IF 模块初始化失败\n");
        close(sig_fd);
        return EXIT_FAILURE;
    }

    /* 使用 epoll 等待信号 */
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        perror("[if] epoll 创建失败");
        if_cleanup();
        close(sig_fd);
        return EXIT_FAILURE;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sig_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sig_fd, &ev);

    printf("[if] IF 进程就绪，等待信号...\n");

    /* 主循环：等待关闭信号或 IPC 关闭请求 */
    while (g_if_local && g_if_local->running)
    {
        struct epoll_event events[1];
        int nfds = epoll_wait(epoll_fd, events, 1, 1000);

        if (nfds > 0 && events[0].data.fd == sig_fd)
        {
            struct signalfd_siginfo siginfo;
            read(sig_fd, &siginfo, sizeof(siginfo));
            printf("[if] 收到信号 %d，准备退出\n", siginfo.ssi_signo);
            break;
        }

        /* 检查 IPC 关闭请求 */
        if (g_if_local && g_if_local->ipc_ctx &&
            ipc_shutdown_requested(g_if_local->ipc_ctx))
        {
            printf("[if] 收到 IPC 关闭请求\n");
            break;
        }
    }

    /* 清理 */
    if_cleanup();
    close(epoll_fd);
    close(sig_fd);

    printf("[if] IF 进程退出\n");
    return EXIT_SUCCESS;
}
