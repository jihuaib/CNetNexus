/**
 * @file   access_console.c
 * @brief  ACCESS console（串口）传输层实现：AF_UNIX 本地通道
 * @author jhb
 * @date   2026/05/30
 */
#include "access_console.h"

#include <glib.h>
#include <libgen.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"

enum
{
    ACCESS_CONSOLE_BACKLOG = 5
};

int access_console_create_listen_sock(const char *path)
{
    if (!path || path[0] == '\0')
    {
        return -1;
    }

    struct sockaddr_un addr;
    if (strlen(path) >= sizeof(addr.sun_path))
    {
        LOG_ERROR("Console socket path too long: %s", path);
        return -1;
    }

    /* 尽力创建父目录（如 /opt/netnexus/run）；已存在则忽略 */
    char dir_buf[256];
    g_strlcpy(dir_buf, path, sizeof(dir_buf));
    char *dir = dirname(dir_buf);
    if (dir && dir[0] != '\0')
    {
        mkdir(dir, 0755);
    }

    /* 移除残留 socket 文件，否则 bind 报 EADDRINUSE */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_PERROR("Failed to create console socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        LOG_PERROR("Failed to bind console socket");
        return -1;
    }

    if (listen(fd, ACCESS_CONSOLE_BACKLOG) < 0)
    {
        close(fd);
        unlink(path);
        LOG_PERROR("Failed to listen console socket");
        return -1;
    }

    return fd;
}
