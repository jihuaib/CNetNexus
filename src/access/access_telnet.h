/**
 * @file   access_telnet.h
 * @brief  ACCESS telnet 传输层：TCP vty 监听 socket
 * @author jhb
 * @date   2026/05/30
 *
 * telnet 承载 vty 远程线，默认端口 23（标准 telnet）。默认不监听，需在 console 上配置
 * `transport input telnet` 后才起监听（见 PB 门控）。后续 ssh 传输另开 access_ssh.c。
 */
#ifndef ACCESS_TELNET_H
#define ACCESS_TELNET_H

#include <stdint.h>

/** telnet/vty 标准监听端口 */
#define ACCESS_TELNET_PORT 23

/**
 * @brief 创建并 bind/listen telnet 监听 socket（TCP，0.0.0.0:port）
 * @param port 监听端口
 * @return 监听 socket fd；失败返回 -1
 */
int access_telnet_create_listen_sock(uint16_t port);

#endif // ACCESS_TELNET_H
