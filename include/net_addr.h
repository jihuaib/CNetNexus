/**
 * @file   net_addr.h
 * @brief  通用 IP 地址类型定义（支持 IPv4/IPv6）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef NET_ADDR_H
#define NET_ADDR_H

#include <glib.h>
#include <netinet/in.h>
#include <stddef.h>

/**
 * @brief 通用 IP 地址结构（支持 IPv4 和 IPv6）
 */
typedef struct net_addr
{
    sa_family_t family; /**< 地址族：AF_INET 或 AF_INET6 */
    union
    {
        struct in_addr v4;  /**< IPv4 地址 */
        struct in6_addr v6; /**< IPv6 地址 */
    } u;
} net_addr_t;

/**
 * @brief 从字符串解析 IP 地址（优先尝试 IPv4，再尝试 IPv6）
 * @param str 输入字符串
 * @param out 输出 net_addr_t
 * @return 0 成功，-1 解析失败
 */
int net_addr_from_str(const char *str, net_addr_t *out);

/**
 * @brief 将 net_addr_t 转为规范字符串
 * @param addr 地址指针
 * @param buf  输出缓冲区（建议 64 字节）
 * @param sz   缓冲区大小
 */
void net_addr_to_str(const net_addr_t *addr, char *buf, size_t sz);

/**
 * @brief 比较两个地址是否相等（family + 二进制内容）
 * @param a 地址 a
 * @param b 地址 b
 * @return TRUE 相等，FALSE 不等
 */
gboolean net_addr_equal(const net_addr_t *a, const net_addr_t *b);

#endif /* NET_ADDR_H */
