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
#include "if.h"
#include "net_addr.h"
#include "route_nhobj.h"

/**
 * @brief 全局静态 nexthop 组（按下一跳分组：多条静态路由共享一个 id + 一次 relay 迭代注册）
 *
 * 下一跳身份键（key）与 RIB 路由的 nexthop 对象键 route_nhobj_key_t 完全一致
 * （protocol=STATIC/BLACKHOLE）。仅**复用 route_nhobj_key_t（key）**。
 *
 * relay 解析结果（网关/出接口）是「两步都要用的 value」——在「添加下一跳」（acquire/解析）时
 * 写入 nexthop 对象，同时 static 全局 nexthop 组本地保存一份用于整组前缀重写；
 * route 注入只带 nexthop_id。
 *
 * 四种模式经 key 统一编码：
 * - 纯 nexthop：nh_type=IP，nexthop 有值，key_ifindex=0
 * - nexthop + interface：nh_type=IP，nexthop 有值，key_ifindex=cfg_ifindex
 * - interface-only：nh_type=IP，nexthop 全零，key_ifindex=cfg_ifindex
 * - null0：nh_type=BLACKHOLE，nexthop 全零，key_ifindex=0
 */
typedef struct route_static_nh
{
    route_nhobj_key_t key;  /**< 复用：下一跳身份键（与 RIB 路由一致；同时作为哈希键） */
    uint32_t nexthop_id;    /**< route_nhobj_acquire 申请的 id，引用归零时 release */
    uint32_t refcount;      /**< 引用本下一跳的静态路由条数 */
    uint8_t has_nexthop;    /**< 1=有 IP 下一跳；0=interface-only/null0 */
    uint8_t is_null0;       /**< 1=null0 黑洞 */
    uint8_t resolved;       /**< 当前可达性（1=可达，0=不可达） */
    uint8_t _pad;           /**< 填充对齐 */
    net_addr_t relay_addr;  /**< relay 解析后的网关（组级 value） */
    uint32_t relay_ifindex; /**< relay 解析后的出接口（组级 value） */
    char out_ifname[IF_LOGICAL_NAME_MAX]; /**< 原始接口名（用于 ifname→ifindex 再解析） */
} route_static_nh_t;

/**
 * @brief 静态路由候选条目键：前缀 + nexthop_id（路由只看到 nexthop id，不存下一跳地址）
 */
typedef struct route_static_entry_key
{
    uint32_t vrf_id;        /**< VRF ID */
    uint16_t afi;           /**< 地址族 */
    uint8_t prefix_len;     /**< 前缀长度 */
    uint8_t _pad;           /**< 填充对齐 */
    net_addr_t prefix_addr; /**< 前缀地址（二进制） */
    uint32_t nexthop_id;    /**< 静态路由的下一跳（以全局静态 nexthop 组的 id 表示） */
} route_static_entry_key_t;

/**
 * @brief 静态路由候选条目（存于全局候选表，等待 nexthop 可达后写入 RIB）
 */
typedef struct route_static_entry
{
    route_static_entry_key_t key; /**< 内嵌键（前缀 + nexthop_id） */
    int32_t metric;               /**< 度量值 */
    int32_t preference;           /**< 管理距离 */
    uint8_t in_rib;               /**< 是否已写入 RIB（1=已写入，0=未写入） */
    uint8_t _pad[3];              /**< 填充对齐 */
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
 * @brief 删除指定 VRF 下所有候选静态路由（VRF 删除级联）
 *
 * 若相关路由已在 RIB 中，则同步撤销并通知订阅者；并注销 relay watch。
 *
 * @param vrf_id VRF ID
 * @return 删除条目数（>=0）
 */
int route_static_del_vrf(uint32_t vrf_id);

/**
 * @brief 指定 nexthop 可达性变化时，更新所有关联的候选静态路由
 *
 * 由 route_relay 在 watch 表检测到 nexthop 状态变化时统一回调（与协议迭代走同一流程）。
 * 遍历候选表中 nexthop_id 匹配的条目，按新状态写入/撤销 RIB。
 * gateway 和 out_ifindex 由 relay 迭代一次性解析完成，无需二次迭代。
 *
 * @param nexthop_id   nexthop 对象 ID
 * @param resolved     新的可达性状态（1=可达，0=不可达）
 * @param gateway      解析出的直连网关地址（仅 resolved=1 时有效）
 * @param out_ifindex  解析出的出接口索引（仅 resolved=1 时有效）
 */
void route_static_on_nh_change(uint32_t nexthop_id, int resolved, const net_addr_t *gateway, uint32_t out_ifindex);

/**
 * @brief 接口状态变化时，重检查所有 interface-only 静态路由的可达性
 *
 * 遍历候选表中 has_nexthop=0 的条目（null0 除外），基于 RIB 中 CONNECTED 路由判断接口可达性。
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
void route_static_show(GString *buf, uint16_t afi_filter, int has_afi_filter, uint32_t vrf_filter,
                       const char *vrf_name);

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
void route_static_show_relay(GString *buf, uint16_t afi_filter, int has_afi_filter, uint32_t vrf_filter,
                             const char *vrf_name);

/**
 * @brief 输出静态 nexthop 组表（按下一跳分组），供 `show route static nexthop`
 *
 * @param buf        输出缓冲区
 * @param has_afi    是否按地址族过滤
 * @param afi        地址族（has_afi 时有效）
 * @param has_vrf    是否按 VRF 过滤
 * @param vrf_id     VRF ID（has_vrf 时有效）
 * @param has_nhid   是否按 nexthop_id 过滤
 * @param nexthop_id nexthop 对象 id（has_nhid 时有效）
 */
void route_static_show_nexthop(GString *buf, int has_afi, uint16_t afi, int has_vrf, uint32_t vrf_id, int has_nhid,
                               uint32_t nexthop_id);

/**
 * @brief 清理候选静态路由表（shutdown 时调用）
 */
void route_static_cleanup(void);

#endif /* ROUTE_STATIC_H */
