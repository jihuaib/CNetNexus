/**
 * @file   bgp_proc.c
 * @brief  BGP 独立进程入口
 * @author jhb
 * @date   2026/03/10
 */
#include <signal.h>
#include <sys/prctl.h>

#include "bgp_main.h"

int main(void)
{
    /* 父进程（DEV）意外退出时自动终止本进程 */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* 忽略 SIGPIPE（TCP 写入已关闭连接时不崩溃） */
    signal(SIGPIPE, SIG_IGN);

    /* 显式初始化模块（IPC 连接、本地状态） */
    if (bgp_module_init() != 0)
    {
        return 1;
    }

    /* 阻塞 SIGTERM/SIGINT，通过 sigwait 捕获关闭信号 */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sig = 0;
    sigwait(&mask, &sig);

    bgp_module_cleanup();

    return 0;
}
