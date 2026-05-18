/**
 * @file   dev_ping.h
 * @brief  Dev 模块自带 ICMP ping 会话：支持 IPv4/IPv6、可选源地址绑定
 * @author jhb
 * @date   2026-05-05
 */
#ifndef DEV_PING_H
#define DEV_PING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net_addr.h"
#include "tunnel.h"

/**
 * @brief Ping 会话句柄（不透明结构）
 */
typedef struct dev_ping_session dev_ping_session_t;

/**
 * @brief 开启一个 ping 会话
 * @param target      目标地址（必填）
 * @param source      源地址（NULL 表示让内核选）
 * @param count       发送探测数（典型 4）
 * @param timeout_ms  单包等待超时（毫秒）
 * @param errmsg      失败原因输出缓冲（可为 NULL）
 * @param errmsg_len  errmsg 缓冲长度
 * @return 会话指针，失败返回 NULL
 */
dev_ping_session_t *dev_ping_start(const net_addr_t *target, const net_addr_t *source, int count, int timeout_ms,
                                   char *errmsg, size_t errmsg_len);

/**
 * @brief 开启一个显式 MPLS IPv4 ping 会话
 * @param target      内层 IPv4 目的地址
 * @param source      内层 IPv4 源地址（NULL 时使用出接口 IPv4）
 * @param tunnel      tunnel resolve 结果（需 resolved 且带 label stack）
 * @param count       发送探测数
 * @param timeout_ms  单包等待超时（毫秒）
 * @param errmsg      失败原因输出缓冲（可为 NULL）
 * @param errmsg_len  errmsg 缓冲长度
 * @return 会话指针，失败返回 NULL
 */
dev_ping_session_t *dev_ping_mpls_start(const net_addr_t *target, const net_addr_t *source,
                                        const tunnel_resolve_notify_t *tunnel, int count, int timeout_ms, char *errmsg,
                                        size_t errmsg_len);

/**
 * @brief 取下一行输出（非阻塞向下推进状态机）
 * @param s        会话指针
 * @param out      输出缓冲（不含末尾 \r\n）
 * @param out_len  缓冲长度
 * @return 1=本行已写入 out，0=会话已结束
 */
int dev_ping_next_line(dev_ping_session_t *s, char *out, size_t out_len);

/**
 * @brief 关闭并释放会话
 */
void dev_ping_close(dev_ping_session_t *s);

#endif /* DEV_PING_H */
