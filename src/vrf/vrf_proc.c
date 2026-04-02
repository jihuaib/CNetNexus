/**
 * @file   vrf_proc.c
 * @brief  VRF 独立进程入口
 * @author jhb
 * @date   2026/03/10
 */
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

#include "vrf_main.h"

static volatile sig_atomic_t g_shutdown = 0;

static void shutdown_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

int main(void)
{
    /* 父进程（DEV）意外退出时自动终止本进程 */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* 忽略 SIGPIPE（TCP 写入已关闭连接时不崩溃） */
    signal(SIGPIPE, SIG_IGN);

    /* 显式初始化模块（IPC 连接、VRF 哈希表） */
    if (vrf_module_init() != 0)
    {
        return 1;
    }

    /*
     * SIGINT/SIGTERM：使用普通信号处理函数（不阻塞）
     *   - 正常运行：信号 → handler 置标志 → sigsuspend 返回 → 优雅退出
     *   - GDB 调试：GDB 拦截 SIGINT → handler 不会被调用 → 程序不退出
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* 等待关闭信号 */
    sigset_t empty;
    sigemptyset(&empty);
    while (!g_shutdown)
    {
        sigsuspend(&empty);
    }

    vrf_module_cleanup();

    return 0;
}
