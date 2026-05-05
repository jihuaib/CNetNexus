/**
 * @file   ldp_proc.c
 * @brief  LDP 独立进程入口
 * @author jhb
 * @date   2026/05/05
 */
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

#include "ldp_main.h"

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

    if (ldp_module_init() != 0)
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

    ldp_module_cleanup();
    return 0;
}
