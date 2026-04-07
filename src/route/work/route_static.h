/**
 * @file   route_static.h
 * @brief  静态路由候选表：存储配置的静态路由，仅在 nexthop 可达时才写入 RIB
 * @author jhb
 * @date   2026-03-21
 */
#ifndef ROUTE_STATIC_H
#define ROUTE_STATIC_H

#include <glib.h>
#include <stdint.h>

#include "dev.h"
#include "if_event.h"
#include "net_addr.h"

/**
 * @brief 候选静态路由条目键（prefix + nexthop + out_ifname 唯一标识一条静态路由）
 *
 * 三种模式：
 * - 纯 nexthop：nexthop_addr 有值，out_ifname 空
 * - nexthop + interface：nexthop_addr 有值，out_ifname 有值
 * - interface-only：nexthop_addr 全零，out_ifname 有值
 */
typedef struct route_static_entry_key
{
    uint32_t vrf_id;                      /**< VRF ID */
    uint16_t afi;                         /**< 地址族（ROUTE_AFI_IPV4 / ROUTE_AFI_IPV6） */
    uint8_t prefix_len;                   /**< 前缀长度 */
    uint8_t _pad;                         /**< 填充对齐 */
    net_addr_t prefix_addr;               /**< 前缀地址（二进制） */
    net_addr_t nexthop_addr;              /**< 下一跳地址（二进制，全零表示 interface-only） */
    char out_ifname[IF_LOGICAL_NAME_MAX]; /**< 出接口逻辑名（空字符串=不约束） */
} route_static_entry_key_t;

/**
 * @brief 候选静态路由条目（存于候选表，等待可达后写入 RIB）
 */
typedef struct route_static_entry
{
    route_static_entry_key_t key; /**< 内嵌键 */
    int32_t metric;               /**< 度量值 */
    int32_t preference;           /**< 管理距离 */
    uint8_t nh_resolved;          /**< nexthop 当前是否可达（1=可达，0=不可达） */
    uint8_t in_rib;               /**< 是否已写入 RIB（1=已写入，0=未写入） */
    uint8_t has_nexthop;          /**< 1=有 nexthop IP，0=interface-only */
    uint8_t _pad[5];              /**< 填充对齐 */
    uint32_t cfg_ifindex;         /**< 配置时/刷新时解析的接口 ifindex */
} route_static_entry_t;

/**
 * @brief 初始化候选静态路由表（在 RIB 创建后调用）
 */
void route_static_init(void);

/**
 * @brief 添加或更新一条候选静态路由
 *
 * 立即检查可达性：若可达则写入 RIB 并通知订阅者；否则仅存入候选表等待迭代。
 * 若条目已存在（prefix+nexthop+ifname 相同），则更新 metric/preference 并重新检查可达性。
 *
 * @param vrf_id       VRF ID
 * @param afi          地址族
 * @param prefix_addr  前缀地址（二进制）
 * @param prefix_len   前缀长度
 * @param nexthop_addr 下一跳地址（二进制，interface-only 时传全零地址）
 * @param metric       度量值
 * @param preference   管理距离
 * @param out_ifname   出接口逻辑名（空字符串或 NULL 表示不约束）
 * @return 0 成功，-1 失败（参数非法或内存不足）
 */
int route_static_add(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                     const net_addr_t *nexthop_addr, int32_t metric, int32_t preference, const char *out_ifname);

/**
 * @brief 删除指定的候选静态路由（prefix + nexthop + ifname 精确匹配）
 *
 * 若该路由已在 RIB 中，则同步撤销并通知订阅者。
 *
 * @param vrf_id       VRF ID
 * @param afi          地址族
 * @param prefix_addr  前缀地址（二进制）
 * @param prefix_len   前缀长度
 * @param nexthop_addr 下一跳地址（二进制，interface-only 时传全零地址）
 * @param out_ifname   出接口逻辑名（空字符串或 NULL 表示不约束）
 * @return 1=删除成功，0=未命中，-1=参数非法
 */
int route_static_del(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                     const net_addr_t *nexthop_addr, const char *out_ifname);

/**
 * @brief 删除某前缀下所有候选静态路由
 *
 * 若相关路由已在 RIB 中，则同步撤销并通知订阅者。
 *
 * @param vrf_id      VRF ID
 * @param afi         地址族
 * @param prefix_addr 前缀地址（二进制）
 * @param prefix_len  前缀长度
 * @return 删除条目数（>=0），-1=参数非法
 */
int route_static_del_prefix(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len);

/**
 * @brief 指定 nexthop 可达性变化时，更新所有关联的候选静态路由
 *
 * 由 route_relay 在 watch 表检测到 nexthop 状态变化时统一回调（与协议迭代走同一流程）。
 * 遍历候选表中 (vrf_id, afi, nexthop_addr) 匹配的条目，按新状态写入/撤销 RIB。
 * gateway 和 out_ifindex 由 relay 迭代一次性解析完成，无需二次迭代。
 *
 * @param vrf_id       VRF ID
 * @param afi          地址族
 * @param nexthop_addr 状态发生变化的下一跳地址
 * @param resolved     新的可达性状态（1=可达，0=不可达）
 * @param gateway      解析出的直连网关地址（仅 resolved=1 时有效）
 * @param out_ifindex  解析出的出接口索引（仅 resolved=1 时有效）
 */
void route_static_on_nh_change(uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop_addr, int resolved,
                               const net_addr_t *gateway, uint32_t out_ifindex);

/**
 * @brief 接口状态变化时，重检查所有 interface-only 静态路由的可达性
 *
 * 遍历候选表中 has_nexthop=0 的条目（Null0 除外），基于 RIB 中 CONNECTED 路由判断接口可达性。
 * 可达则写入 RIB，不可达则从 RIB 撤销。
 */
void route_static_on_if_change(void);

/**
 * @brief 格式化输出候选静态路由表到缓冲区
 *
 * 每条记录显示：前缀、nexthop、metric、preference、nh_resolved（迭代是否可达）、in_rib（是否已写入 RIB）。
 *
 * @param buf            输出 GString 缓冲区（不可为 NULL）
 * @param afi_filter     地址族过滤（ROUTE_AFI_IPV4/IPV6）
 * @param has_afi_filter 非零表示启用 afi_filter 过滤
 */
void route_static_show(GString *buf, uint16_t afi_filter, int has_afi_filter);

/**
 * @brief 以 relay 风格输出候选静态路由的 nexthop 迭代状态（按 nexthop 聚合）
 *
 * 每行代表一个唯一的 nexthop，显示 VRF、AFI、nexthop 地址、依赖该 nexthop 的路由数量及可达性。
 * 供 show route relay static 命令调用。
 *
 * @param buf            输出 GString 缓冲区（不可为 NULL）
 * @param afi_filter     地址族过滤（ROUTE_AFI_IPV4/IPV6）
 * @param has_afi_filter 非零表示启用 afi_filter 过滤
 */
void route_static_show_relay(GString *buf, uint16_t afi_filter, int has_afi_filter);

/**
 * @brief 清理候选静态路由表（shutdown 时调用）
 */
void route_static_cleanup(void);

#endif /* ROUTE_STATIC_H */
