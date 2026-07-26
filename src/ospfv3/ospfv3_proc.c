/**
 * @file   ospfv3_proc.c
 * @brief  OSPFv3 standalone process entry point
 */
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

#include "ospfv3_main.h"

static volatile sig_atomic_t g_shutdown;

static void ospfv3_shutdown_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

int main(void)
{
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    signal(SIGPIPE, SIG_IGN);

    if (ospfv3_module_init() != 0)
    {
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ospfv3_shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    sigset_t empty;
    sigemptyset(&empty);
    while (!g_shutdown)
    {
        sigsuspend(&empty);
    }

    ospfv3_module_cleanup();
    return 0;
}
