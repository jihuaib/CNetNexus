/**
 * @file   main.c
 * @brief  进程管理器，负责启动、监控和关闭所有子进程
 * @author jhb
 * @date   2026/02/03
 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "path_utils.h"

// ============================================================================
// 子进程定义
// ============================================================================

/** 最大子进程数 */
#define MAX_CHILDREN 8

/** 子进程关闭超时（秒） */
#define SHUTDOWN_TIMEOUT_SEC 5

/** 子进程重启延迟（秒） */
#define RESTART_DELAY_SEC 2

/**
 * @brief 子进程描述
 */
typedef struct child_proc
{
    const char *name;  /**< 进程名（如 "db"） */
    pid_t pid;         /**< 进程 ID（0 表示未启动） */
    int restart;       /**< 是否在崩溃后自动重启 */
    int started;       /**< 是否已成功启动 */
} child_proc_t;

/** 子进程列表，按启动依赖顺序排列 */
static child_proc_t g_children[] = {
    {"db", 0, 1, 0},
    {"cfg", 0, 1, 0},
    {"bgp", 0, 1, 0},
    {"if", 0, 1, 0},
    {"access", 0, 1, 0},
    {"route", 0, 1, 0},
};

static const int g_num_children = sizeof(g_children) / sizeof(g_children[0]);

/** 全局关闭标志 */
static volatile int g_shutting_down = 0;

// ============================================================================
// 子进程管理
// ============================================================================

/**
 * @brief 启动一个子进程
 * @param child 子进程描述
 * @param exe_dir 可执行文件目录
 * @return 成功返回 0，失败返回 -1
 */
static int start_child(child_proc_t *child, const char *exe_dir)
{
    char exe_path[PATH_MAX];
    snprintf(exe_path, sizeof(exe_path), "%s/%s", exe_dir, child->name);

    /* 检查可执行文件是否存在 */
    if (access(exe_path, X_OK) != 0)
    {
        fprintf(stderr, "[netnexus] 可执行文件不存在或无执行权限: %s\n", exe_path);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        perror("[netnexus] fork 失败");
        return -1;
    }

    if (pid == 0)
    {
        /* 子进程：解除信号阻塞（继承自父进程） */
        sigset_t mask;
        sigemptyset(&mask);
        sigprocmask(SIG_SETMASK, &mask, NULL);

        /* exec 子进程 */
        execl(exe_path, child->name, (char *)NULL);

        /* exec 失败 */
        perror(child->name);
        _exit(EXIT_FAILURE);
    }

    /* 父进程 */
    child->pid = pid;
    child->started = 1;
    printf("[netnexus] 启动 %s (pid=%d)\n", child->name, pid);

    return 0;
}

/**
 * @brief 启动所有子进程
 * @param exe_dir 可执行文件目录
 * @return 成功启动的数量
 */
static int start_all_children(const char *exe_dir)
{
    int started = 0;

    for (int i = 0; i < g_num_children; i++)
    {
        if (start_child(&g_children[i], exe_dir) == 0)
        {
            started++;

            /* db 和 cfg 启动后短暂等待，让它们初始化 */
            if (i <= 1)
            {
                usleep(500000); /* 500ms */
            }
            else
            {
                usleep(100000); /* 100ms */
            }
        }
        else
        {
            fprintf(stderr, "[netnexus] 启动 %s 失败\n", g_children[i].name);
        }
    }

    return started;
}

/**
 * @brief 根据 PID 查找子进程
 * @param pid 进程 ID
 * @return 子进程描述，未找到返回 NULL
 */
static child_proc_t *find_child_by_pid(pid_t pid)
{
    for (int i = 0; i < g_num_children; i++)
    {
        if (g_children[i].pid == pid)
        {
            return &g_children[i];
        }
    }
    return NULL;
}

/**
 * @brief 处理子进程退出（SIGCHLD）
 * @param exe_dir 可执行文件目录（用于重启）
 */
static void handle_sigchld(const char *exe_dir)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        child_proc_t *child = find_child_by_pid(pid);
        if (!child)
        {
            continue;
        }

        child->pid = 0;

        if (WIFEXITED(status))
        {
            printf("[netnexus] %s 退出 (exit code=%d)\n", child->name, WEXITSTATUS(status));
        }
        else if (WIFSIGNALED(status))
        {
            printf("[netnexus] %s 被信号终止 (signal=%d)\n", child->name, WTERMSIG(status));
        }

        /* 非关闭状态下自动重启 */
        if (!g_shutting_down && child->restart)
        {
            printf("[netnexus] %d 秒后重启 %s...\n", RESTART_DELAY_SEC, child->name);
            sleep(RESTART_DELAY_SEC);
            if (!g_shutting_down)
            {
                start_child(child, exe_dir);
            }
        }
    }
}

/**
 * @brief 优雅关闭所有子进程
 *
 * 先发送 SIGTERM，等待超时后发送 SIGKILL。
 */
static void shutdown_all_children(void)
{
    g_shutting_down = 1;

    printf("[netnexus] 正在关闭所有子进程...\n");

    /* 逆序发送 SIGTERM（先关闭依赖方） */
    for (int i = g_num_children - 1; i >= 0; i--)
    {
        if (g_children[i].pid > 0)
        {
            printf("[netnexus] 发送 SIGTERM 到 %s (pid=%d)\n", g_children[i].name, g_children[i].pid);
            kill(g_children[i].pid, SIGTERM);
        }
    }

    /* 等待子进程退出，超时后强制终止 */
    int remaining = SHUTDOWN_TIMEOUT_SEC;
    while (remaining > 0)
    {
        /* 检查是否还有存活的子进程 */
        int alive = 0;
        for (int i = 0; i < g_num_children; i++)
        {
            if (g_children[i].pid > 0)
            {
                int status;
                pid_t result = waitpid(g_children[i].pid, &status, WNOHANG);
                if (result > 0)
                {
                    printf("[netnexus] %s 已退出\n", g_children[i].name);
                    g_children[i].pid = 0;
                }
                else
                {
                    alive++;
                }
            }
        }

        if (alive == 0)
        {
            break;
        }

        printf("[netnexus] 等待 %d 个子进程退出 (%d 秒后强制终止)...\n", alive, remaining);
        sleep(1);
        remaining--;
    }

    /* 超时后强制终止剩余进程 */
    for (int i = 0; i < g_num_children; i++)
    {
        if (g_children[i].pid > 0)
        {
            fprintf(stderr, "[netnexus] 强制终止 %s (pid=%d)\n", g_children[i].name, g_children[i].pid);
            kill(g_children[i].pid, SIGKILL);
            waitpid(g_children[i].pid, NULL, 0);
            g_children[i].pid = 0;
        }
    }
}

// ============================================================================
// 主入口
// ============================================================================

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("NetNexus 进程管理器启动 (pid=%d)\n", getpid());
    printf("========================================\n");

    /* 获取可执行文件目录 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
    {
        fprintf(stderr, "[netnexus] 无法获取可执行文件目录\n");
        return EXIT_FAILURE;
    }
    printf("[netnexus] 可执行文件目录: %s\n", exe_dir);

    /* 阻塞 SIGINT、SIGTERM、SIGCHLD，使用 signalfd 统一处理 */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
    {
        perror("[netnexus] sigprocmask");
        return EXIT_FAILURE;
    }

    int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (signal_fd == -1)
    {
        perror("[netnexus] signalfd");
        return EXIT_FAILURE;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1)
    {
        perror("[netnexus] epoll_create1");
        close(signal_fd);
        return EXIT_FAILURE;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = signal_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd, &ev) == -1)
    {
        perror("[netnexus] epoll_ctl");
        close(signal_fd);
        close(epoll_fd);
        return EXIT_FAILURE;
    }

    /* 启动所有子进程 */
    int started = start_all_children(exe_dir);
    if (started == 0)
    {
        fprintf(stderr, "[netnexus] 没有子进程成功启动\n");
        close(signal_fd);
        close(epoll_fd);
        return EXIT_FAILURE;
    }

    printf("\n[netnexus] %d/%d 个进程已启动。按 Ctrl+C 关闭。\n\n", started, g_num_children);

    /* 主事件循环 */
    struct epoll_event events[4];
    while (!g_shutting_down)
    {
        int nfds = epoll_wait(epoll_fd, events, 4, -1);
        if (nfds == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("[netnexus] epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            if (events[i].data.fd != signal_fd)
            {
                continue;
            }

            struct signalfd_siginfo si;
            ssize_t s = read(signal_fd, &si, sizeof(si));
            if (s != sizeof(si))
            {
                continue;
            }

            switch (si.ssi_signo)
            {
                case SIGCHLD:
                    handle_sigchld(exe_dir);
                    break;

                case SIGINT:
                case SIGTERM:
                    printf("\n[netnexus] 收到信号 %d，开始关闭...\n", si.ssi_signo);
                    shutdown_all_children();
                    break;

                default:
                    break;
            }
        }
    }

    close(signal_fd);
    close(epoll_fd);

    printf("\nNetNexus 已关闭\n");
    return EXIT_SUCCESS;
}
