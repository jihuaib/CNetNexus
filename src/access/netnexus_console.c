/**
 * @file   netnexus_console.c
 * @brief  ACCESS console（串口）客户端：把本地终端桥接到 ACCESS 的 console unix socket
 * @author jhb
 * @date   2026/05/30
 *
 * 用途：console 通道是 AF_UNIX socket，telnet 连不了，故自带这个迷你客户端。
 *       它不含任何 CLI 逻辑——回显/行编辑/提示符都在 ACCESS（line 层）做，
 *       本程序只负责：连接 socket、把本地终端置 raw、双向逐字节转发 stdin↔socket。
 *
 *   gns3-entry.sh: exec netnexus-console        （GNS3 节点 console 即此）
 *   手动:          docker exec -it <ctr> netnexus-console
 *   退出:          按 Ctrl-]（0x1d）或对端关闭连接
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include "access.h"

/* Ctrl-] 退出键（与 telnet 习惯一致） */
#define CONSOLE_ESCAPE_CHAR 0x1d

static struct termios g_saved_termios;
static int g_termios_saved = 0;

static void restore_termios(void)
{
    if (g_termios_saved)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
        g_termios_saved = 0;
    }
}

int main(void)
{
    const char *path = access_console_sock_path();

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "netnexus-console: cannot connect to %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    /* 本地终端置 raw（关回显/行缓冲），逐字节透传给 ACCESS */
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_saved_termios) == 0)
    {
        struct termios raw = g_saved_termios;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
        {
            g_termios_saved = 1;
        }
    }

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = fd;
    fds[1].events = POLLIN;

    char buf[4096];
    int running = 1;
    while (running)
    {
        if (poll(fds, 2, -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if (fds[0].revents & POLLIN)
        {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0)
            {
                break;
            }
            /* 检测退出键 Ctrl-] */
            for (ssize_t i = 0; i < n; i++)
            {
                if ((unsigned char)buf[i] == CONSOLE_ESCAPE_CHAR)
                {
                    running = 0;
                    break;
                }
            }
            if (!running)
            {
                break;
            }
            if (write(fd, buf, (size_t)n) < 0)
            {
                break;
            }
        }

        if (fds[1].revents & POLLIN)
        {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0)
            {
                break; /* 对端关闭（会话退出） */
            }
            if (write(STDOUT_FILENO, buf, (size_t)n) < 0)
            {
                break;
            }
        }

        if (fds[1].revents & (POLLHUP | POLLERR))
        {
            break;
        }
    }

    restore_termios();
    close(fd);
    printf("\r\nConsole disconnected.\r\n");
    return 0;
}
