/**
 * @file   bgp_listen.c
 * @brief  BGP 全局监听 socket 创建与销毁（绑定 0.0.0.0:179）
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_listen.h"

#include <arpa/inet.h>
#include <glib.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"

/** BGP 协议标准端口 */
#define BGP_LISTEN_PORT 179

bgp_listen_t *bgp_listen_create(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_PERROR("BGP: 创建全局 listen socket 失败");
        return NULL;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(BGP_LISTEN_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_PERROR("BGP: bind 0.0.0.0:179 失败");
        close(fd);
        return NULL;
    }

    if (listen(fd, 32) < 0)
    {
        LOG_PERROR("BGP: listen 失败");
        close(fd);
        return NULL;
    }

    bgp_listen_t *l = g_malloc0(sizeof(bgp_listen_t));
    l->fd = fd;
    LOG_INFO("BGP: 全局 listen socket 已创建，监听 0.0.0.0:179 (fd=%d)", fd);
    return l;
}

void bgp_listen_destroy(bgp_listen_t *l)
{
    if (!l)
    {
        return;
    }
    if (l->fd >= 0)
    {
        close(l->fd);
        l->fd = -1;
    }
    g_free(l);
}
