/**
 * @file   access_console.h
 * @brief  ACCESS console（串口）传输层：AF_UNIX 本地通道，永远在线
 * @author jhb
 * @date   2026/05/30
 *
 * console 是兜底登录入口（串口语义），独立于 telnet/vty：用 Unix domain socket 承载，
 * 由 netnexus-console 客户端桥接本地终端。后续真硬件串口可在此扩展 open(/dev/ttySx)。
 */
#ifndef ACCESS_CONSOLE_H
#define ACCESS_CONSOLE_H

/**
 * @brief 创建并 bind/listen console 监听 socket（AF_UNIX SOCK_STREAM）
 * @param path unix socket 路径（bind 前会 unlink 残留并尽力 mkdir 父目录）
 * @return 监听 socket fd；失败返回 -1
 */
int access_console_create_listen_sock(const char *path);

#endif // ACCESS_CONSOLE_H
