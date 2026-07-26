/**
 * @file   ospf_proc.c
 * @brief  OSPFv2 standalone process entry point
 */
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

#include "ospf_main.h"

static volatile sig_atomic_t g_shutdown;

static void ospf_shutdown_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

int main(void)
{
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    signal(SIGPIPE, SIG_IGN);

    if (ospf_module_init() != 0)
    {
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ospf_shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    sigset_t empty;
    sigemptyset(&empty);
    while (!g_shutdown)
    {
        sigsuspend(&empty);
    }

    ospf_module_cleanup();
    return 0;
}
