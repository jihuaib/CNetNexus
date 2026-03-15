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

/**
 * @brief 按字典序比较两个地址（family 优先，再比较地址字节）
 * @param a 地址 a
 * @param b 地址 b
 * @return <0 表示 a<b，0 表示相等，>0 表示 a>b
 */
int net_addr_cmp(const net_addr_t *a, const net_addr_t *b);

/**
 * @brief GLib 哈希表用 hash 函数（key 为 net_addr_t*）
 * @param key net_addr_t 指针
 * @return 哈希值
 */
guint net_addr_hash(gconstpointer key);

/**
 * @brief GLib 哈希表用 equal 函数（按值比较 net_addr_t）
 * @param a net_addr_t 指针 a
 * @param b net_addr_t 指针 b
 * @return TRUE 相等，FALSE 不等
 */
gboolean net_addr_hash_equal(gconstpointer a, gconstpointer b);

// ============================================================================
// IP 前缀（地址 + 前缀长度）
// ============================================================================

/**
 * @brief IP 地址 + 前缀长度（CIDR 表示），可作为通用 IP 配置结构
 */
typedef struct
{
    net_addr_t addr;    /**< IP 地址；family=0 表示未配置 */
    uint8_t prefix_len; /**< 前缀长度（IPv4: 0-32，IPv6: 0-128） */
} net_prefix_t;

/**
 * @brief 判断前缀是否已配置（addr.family != 0）
 * @param p 前缀指针
 * @return TRUE 已配置，FALSE 未配置
 */
gboolean net_prefix_is_set(const net_prefix_t *p);

/**
 * @brief 将前缀转为 "addr/len" 字符串，如 "192.168.1.1/24"
 * @param p   前缀指针
 * @param buf 输出缓冲区（建议 70 字节）
 * @param sz  缓冲区大小
 */
void net_prefix_to_str(const net_prefix_t *p, char *buf, size_t sz);

/**
 * @brief 将前缀长度转为点分十进制掩码字符串（仅 IPv4）
 * @param prefix_len 前缀长度（0-32）
 * @param buf        输出缓冲区（至少 16 字节）
 */
void net_prefix_len_to_mask_str(uint8_t prefix_len, char *buf);

#endif /* NET_ADDR_H */
