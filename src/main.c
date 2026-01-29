#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "dev/nn_dev_module.h"
#include "nn_dev.h"
#include "nn_errcode.h"

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
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    // Create signalfd to receive signals
    signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (signal_fd == -1)
    {
        perror("signalfd");
        return EXIT_FAILURE;
    }

    // Create epoll instance
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1)
    {
        perror("epoll_create1");
        close(signal_fd);
        return EXIT_FAILURE;
    }

    // Add signalfd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = signal_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd, &ev) == -1)
    {
        perror("epoll_ctl");
        close(signal_fd);
        close(epoll_fd);
        return EXIT_FAILURE;
    }

    // Initialize all registered modules
    if (nn_dev_init_all_modules() != NN_ERRCODE_SUCCESS)
    {
        fprintf(stderr, "Warning: Some modules failed to initialize\n");
    }

    printf("All modules initialized. Press Ctrl+C to stop.\n\n");

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
            perror("epoll_wait");
            break;
        }

        if (nfds > 0)
        {
            // Signal received on signalfd
            struct signalfd_siginfo si;
            ssize_t s = read(signal_fd, &si, sizeof(si));
            if (s == sizeof(si))
            {
                printf("\nReceived signal %d, requesting shutdown...\n", si.ssi_signo);
                nn_dev_request_shutdown();
                break;
            }
        }
    }

    // Cleanup
    close(signal_fd);
    close(epoll_fd);

    // Cleanup all modules
    nn_cleanup_all_modules();

    printf("\nNetNexus shutdown complete\n");
    return EXIT_SUCCESS;
}
