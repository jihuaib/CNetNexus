/**
 * @file   snmp_proc.c
 * @brief  SNMP 独立进程入口
 */
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

#include "snmp_main.h"

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

    if (snmp_module_init() != 0)
    {
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /*
     * DEV starts children with termination signals blocked.  Only the main
     * thread needs to accept them: process-directed SIGTERM from the CLI
     * worker will then wake this loop and drive orderly module cleanup.
     */
    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGTERM);
    sigaddset(&shutdown_signals, SIGINT);
    (void)pthread_sigmask(SIG_UNBLOCK, &shutdown_signals, NULL);

    snmp_agent_loop(&g_shutdown);
    snmp_module_cleanup();
    return 0;
}
