/**
 * @file   access_telnet.c
 * @brief  ACCESS telnet 传输层实现（TCP vty）
 * @author jhb
 * @date   2026/05/30
 */
#include "access_telnet.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"

enum
{
    ACCESS_TELNET_BACKLOG = 5
};

int access_telnet_create_listen_sock(uint16_t port)
{
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        LOG_PERROR("Failed to create socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        close(server_socket);
        LOG_PERROR("Failed to set socket options");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    /* execv 重启后旧进程刚 close 的监听端口需短暂时间才被内核完全释放，
     * SO_REUSEADDR 解决不了此窗口，这里对 EADDRINUSE 做有限重试（≤2s）。 */
    int bind_rc;
    int bind_attempts = 0;
    const int bind_max_attempts = 10;
    while ((bind_rc = bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr))) < 0)
    {
        if (errno != EADDRINUSE || ++bind_attempts >= bind_max_attempts)
        {
            break;
        }
        if (bind_attempts == 1)
        {
            LOG_WARN("Telnet port %u busy, retrying bind (likely post-exec port drain)", port);
        }
        usleep(200 * 1000);
    }
    if (bind_rc < 0)
    {
        close(server_socket);
        LOG_PERROR("Failed to bind socket");
        return -1;
    }

    if (listen(server_socket, ACCESS_TELNET_BACKLOG) < 0)
    {
        close(server_socket);
        LOG_PERROR("Failed to listen");
        return -1;
    }

    return server_socket;
}
