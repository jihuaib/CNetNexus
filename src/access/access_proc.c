/**
 * @file   access_proc.c
 * @brief  ACCESS 独立进程入口
 * @author jhb
 * @date   2026/05/30
 */
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

#include "access_main.h"

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

    if (access_module_init() != 0)
    {
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    sigset_t empty;
    sigemptyset(&empty);
    while (!g_shutdown)
    {
        sigsuspend(&empty);
    }

    access_module_cleanup();

    return 0;
}
