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
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dev/dev_main.h"
#include "dev/dev_module.h"
#include "dev/dev_subscribe.h"
#include "errcode.h"
#include "log.h"

/**
 * 放开 core dump 限制并标记进程可 dump。
 *
 * 注意：core 实际落盘位置由内核全局参数 /proc/sys/kernel/core_pattern 决定，
 * 容器/GNS3 场景下该参数继承自宿主机，无法在容器内独立修改。本函数只能保证
 * 进程自身的 RLIMIT_CORE 和 dumpable 标志到位，cwd 由启动脚本切到可持久化目录。
 */
static void enable_core_dump(void)
{
    struct rlimit rl;
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_CORE, &rl) == -1)
    {
        /* 容器若未传 --ulimit core=-1，硬上限会卡在 0，这里只能告警 */
        LOG_WARN("setrlimit(RLIMIT_CORE) failed: %s (need --ulimit core=-1 in docker)", strerror(errno));
    }
    /* setuid/seccomp 等场景下 dumpable 可能被清零，显式置回 1 */
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1)
    {
        LOG_WARN("prctl(PR_SET_DUMPABLE) failed: %s", strerror(errno));
    }
}

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
    ssize_t n = write(g_shutdown_pipe[1], &c, 1);
    (void)n;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    log_set_tag("main");

    /* 注册主线程日志文件：$NN_WORK_DIR/log/main.log（未设置 NN_WORK_DIR 时仅输出到 stderr） */
    log_register_module_auto("main");

    /* 异常退出时尽可能产出 core dump，方便事后定位 */
    enable_core_dump();

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
    sigaddset(&mask, SIGCHLD); /* 子进程退出通知 */

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
                struct signalfd_siginfo si;
                ssize_t s = read(signal_fd, &si, sizeof(si));
                if (s == sizeof(si))
                {
                    if (si.ssi_signo == SIGTERM)
                    {
                        LOG_INFO("Received SIGTERM, requesting shutdown...");
                        shutdown = 1;
                    }
                    else if (si.ssi_signo == SIGCHLD)
                    {
                        /* 回收所有已退出的子进程（可能一次到达多个 SIGCHLD 但只读一次）。
                         * 若 cleanup_all_modules / dev_reboot_software 正在主动关闭，
                         * 仅回收 + 记录，不做"crashed/respawn"判定。 */
                        int wstatus;
                        pid_t dead;
                        int in_cleanup = dev_module_is_cleanup_in_progress();
                        while ((dead = waitpid(-1, &wstatus, WNOHANG)) > 0)
                        {
                            dev_module_t *m = dev_module_find_by_pid(dead);
                            if (!m)
                            {
                                LOG_WARN("Reaped unknown child pid=%d (status=%d)", dead, wstatus);
                                continue;
                            }

                            LOG_WARN("Module %s (pid=%d) exited (status=%d, on_demand=%u, pending_restart=%u, "
                                     "pending_stop=%u%s)",
                                     m->name, dead, wstatus, m->on_demand, m->pending_restart, m->pending_stop,
                                     in_cleanup ? ", cleanup" : "");

                            if (in_cleanup)
                            {
                                /* 主动关闭路径：cleanup_all_modules 会自己 reap + 释放结构体，
                                 * 这里仅回收 pid 防止僵尸；不要改 phase/child_pid，更不要 respawn。 */
                                continue;
                            }

                            /* 通知订阅者：模块下线 */
                            dev_subscribe_broadcast_event(m, DEV_MODULE_EVENT_DOWN);

                            /* 删除 IPC 连接记录：避免老 conn 残留的 backoff 拖慢下次拉起
                             * （否则 next dev_ipc_connect 会命中旧 conn 而不重置计时器，
                             *  新进程的 init 等待窗口可能与 IO 线程的 10s 重连周期错过）*/
                            if (g_dev_local && g_dev_local->dev_ipc_ctx)
                            {
                                dev_ipc_drop_connection(g_dev_local->dev_ipc_ctx, m->module_id);
                            }

                            m->child_pid = 0;
                            m->phase = DEV_PHASE_REGISTERED;

                            if (m->pending_stop)
                            {
                                /* 用户主动 stop：保持 REGISTERED，不重启、不告警 */
                                m->pending_stop = 0;
                                LOG_INFO("Module %s stopped by user", m->name);
                            }
                            else if (m->pending_restart)
                            {
                                m->pending_restart = 0;
                                LOG_INFO("Module %s: pending_restart, respawning...", m->name);
                                if (dev_module_respawn(m) == ERRCODE_SUCCESS)
                                {
                                    if (g_dev_local && g_dev_local->dev_ipc_ctx)
                                    {
                                        dev_ipc_connect(g_dev_local->dev_ipc_ctx, m->module_id, DEV_IPC_HOST_LOCAL,
                                                        m->port);
                                    }
                                }
                                else
                                {
                                    LOG_ERROR("Module %s respawn failed", m->name);
                                }
                            }
                            else if (!m->on_demand)
                            {
                                LOG_ERROR("Basic module %s crashed; manual intervention required", m->name);
                            }
                        }
                    }
                }
            }
            else if (events[i].data.fd == g_shutdown_pipe[0])
            {
                // SIGINT received via self-pipe
                char buf[16];
                ssize_t n = read(g_shutdown_pipe[0], buf, sizeof(buf));
                (void)n;
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
