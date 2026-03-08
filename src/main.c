/**
 * @file   main.c
 * @brief  主程序入口，TCP 服务器、线程管理和信号处理
 * @author jhb
 * @date   2026/01/22
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "dev/dev_main.h"
#include "dev/dev_module.h"
#include "errcode.h"
#include "log.h"

/**
 * Self-pipe：用于将 SIGINT 信号通知到 epoll 事件循环。
 *
 * 为什么不用 signalfd 处理 SIGINT：
 *   signalfd 要求信号被 sigprocmask 阻塞，阻塞后信号直接进入内核
 *   pending 队列，GDB 的 nopass 无法阻止 signalfd 读到该信号。
 *   改用普通信号处理函数，GDB 能在信号交付前拦截，从而让
 *   Ctrl+C 只暂停程序而不触发退出。
 */
static int g_shutdown_pipe[2] = {-1, -1};
static void sigint_handler(int sig)
{
    (void)sig;
    char c = 1;
    /* 信号处理函数中只能调用 async-signal-safe 函数 */
    write(g_shutdown_pipe[1], &c, 1);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    log_set_tag("main");

    /* 若设置了 NN_WORK_DIR，则将日志写入 $NN_WORK_DIR/log/netnexus.log */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        char log_path[512];
        snprintf(log_path, sizeof(log_path), "%s/log/netnexus.log", work_dir);
        log_open_file(log_path);
    }

    int epoll_fd = -1;
    int signal_fd = -1;

    /* 忽略 SIGPIPE，防止向已关闭的 socket 写入时崩溃 */
    signal(SIGPIPE, SIG_IGN);
    /* 创建 self-pipe，用于 SIGINT → epoll 通知 */
    if (pipe2(g_shutdown_pipe, O_CLOEXEC | O_NONBLOCK) == -1)
    {
        LOG_PERROR("pipe2");
        return EXIT_FAILURE;
    }
    /*
     * SIGINT：使用普通信号处理函数（不阻塞、不走 signalfd）
     *   - 正常运行：Ctrl+C → handler 写 pipe → epoll 唤醒 → 优雅退出
     *   - GDB 调试：GDB 拦截 SIGINT → handler 不会被调用 → 程序不退出
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        LOG_PERROR("sigaction");
        return EXIT_FAILURE;
    }
    /* SIGTERM：仍然使用 signalfd（不需要 GDB 兼容） */

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
    {
        LOG_PERROR("sigprocmask");
        return EXIT_FAILURE;
    }

    // Create signalfd for SIGTERM
    signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (signal_fd == -1)
    {
        LOG_PERROR("signalfd");
        return EXIT_FAILURE;
    }

    // Create epoll instance
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1)
    {
        LOG_PERROR("epoll_create1");
        close(signal_fd);
        return EXIT_FAILURE;
    }

    // Add signalfd to epoll (for SIGTERM)
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = signal_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd, &ev) == -1)
    {
        LOG_PERROR("epoll_ctl: signalfd");
        close(signal_fd);
        close(epoll_fd);
        return EXIT_FAILURE;
    }

    // Add shutdown pipe to epoll (for SIGINT)
    ev.events = EPOLLIN;
    ev.data.fd = g_shutdown_pipe[0];
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_shutdown_pipe[0], &ev) == -1)
    {
        LOG_PERROR("epoll_ctl: shutdown_pipe");
        close(signal_fd);
        close(epoll_fd);
        return EXIT_FAILURE;
    }

    // 扫描 module.conf，动态加载所有模块（dlopen + 入口函数）
    if (dev_scan_and_load_modules() != ERRCODE_SUCCESS)
    {
        LOG_WARN("Some modules failed to load");
    }

    // 三阶段初始化所有模块
    if (dev_init_all_modules() != ERRCODE_SUCCESS)
    {
        LOG_WARN("Some modules failed to initialize");
    }

    LOG_INFO("All modules initialized. Press Ctrl+C to stop.");

    // Main event loop - wait for shutdown signal via epoll
    struct epoll_event events[2];
    int shutdown = 0;
    while (!shutdown)
    {
        int nfds = epoll_wait(epoll_fd, events, 2, -1);
        if (nfds == -1)
        {
            if (errno == EINTR)
            {
                continue; // Interrupted by signal, retry
            }
            LOG_PERROR("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            if (events[i].data.fd == signal_fd)
            {
                // SIGTERM received via signalfd
                struct signalfd_siginfo si;
                ssize_t s = read(signal_fd, &si, sizeof(si));
                if (s == sizeof(si))
                {
                    LOG_INFO("Received SIGTERM (%d), requesting shutdown...", si.ssi_signo);
                    shutdown = 1;
                }
            }
            else if (events[i].data.fd == g_shutdown_pipe[0])
            {
                // SIGINT received via self-pipe
                char buf[16];
                read(g_shutdown_pipe[0], buf, sizeof(buf));
                LOG_INFO("Received SIGINT, requesting shutdown...");
                shutdown = 1;
            }
        }
    }

    // Cleanup
    close(signal_fd);
    close(epoll_fd);
    close(g_shutdown_pipe[0]);
    close(g_shutdown_pipe[1]);

    // 清理所有模块（包含逆序 shutdown RPC + IPC 销毁）
    cleanup_all_modules();

    // DEV 本地状态清理
    dev_cleanup_self();

    LOG_INFO("NetNexus shutdown complete");
    return EXIT_SUCCESS;
}
