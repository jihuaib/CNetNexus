/**
 * @file   bgp_listen.h
 * @brief  BGP 全局监听 socket 管理（0.0.0.0:179）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef BGP_LISTEN_H
#define BGP_LISTEN_H

/**
 * @brief BGP 全局监听结构体（单一 0.0.0.0:179 listen socket）
 */
typedef struct bgp_listen
{
    int fd; /**< 绑定 0.0.0.0:179 的 listen socket fd */
} bgp_listen_t;

/**
 * @brief 创建并绑定全局 BGP listen socket（0.0.0.0:179）
 * @return 成功返回 bgp_listen_t 指针，失败返回 NULL
 */
bgp_listen_t *bgp_listen_create(void);

/**
 * @brief 销毁全局 BGP listen socket
 * @param l bgp_listen_t 指针（允许为 NULL）
 */
void bgp_listen_destroy(bgp_listen_t *l);

#endif /* BGP_LISTEN_H */
