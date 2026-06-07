/**
 * @file   lldp_proc.c
 * @brief  LLDP 独立进程入口
 * @author jhb
 * @date   2026/06/07
 */
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

#include "lldp_main.h"

static volatile sig_atomic_t g_shutdown = 0;

static void shutdown_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

int main(void)
{
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    signal(SIGPIPE, SIG_IGN);

    if (lldp_module_init() != 0)
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

    lldp_module_cleanup();
    return 0;
}
