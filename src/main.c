/**
 * @file   main.c
 * @brief  主程序入口，TCP 服务器、线程管理和信号处理
 * @author jhb
 * @date   2026/01/22
 */
#define LOG_TAG "main"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "dev.h"
#include "dev/dev_main.h"
#include "dev/dev_module.h"
#include "errcode.h"
#include "log.h"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int epoll_fd = -1;
    int signal_fd = -1;

    // Block SIGINT and SIGTERM - we'll handle them via signalfd
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
    {
        LOG_PERROR("sigprocmask");
        return EXIT_FAILURE;
    }

    // Create signalfd to receive signals
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

    // Add signalfd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = signal_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd, &ev) == -1)
    {
        LOG_PERROR("epoll_ctl");
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
    struct epoll_event events[1];
    while (1)
    {
        int nfds = epoll_wait(epoll_fd, events, 1, -1);
        if (nfds == -1)
        {
            if (errno == EINTR)
            {
                continue; // Interrupted by signal, retry
            }
            LOG_PERROR("epoll_wait");
            break;
        }

        if (nfds > 0)
        {
            // Signal received on signalfd
            struct signalfd_siginfo si;
            ssize_t s = read(signal_fd, &si, sizeof(si));
            if (s == sizeof(si))
            {
                LOG_INFO("Received signal %d, requesting shutdown...", si.ssi_signo);
                dev_request_shutdown();
                break;
            }
        }
    }

    // Cleanup
    close(signal_fd);
    close(epoll_fd);

    // 清理所有模块（包含逆序 shutdown RPC + IPC 销毁）
    cleanup_all_modules();

    // DEV 本地状态清理
    dev_cleanup_self();

    LOG_INFO("NetNexus shutdown complete");
    return EXIT_SUCCESS;
}
