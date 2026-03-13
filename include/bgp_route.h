/**
 * @file   bgp_route.h
 * @brief  BGP 路由公共数据结构：前缀、属性、nexthop、NLRI 条目、UPDATE 解析结果
 * @author jhb
 * @date   2026/03/11
 */
#ifndef BGP_ROUTE_H
#define BGP_ROUTE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "net_addr.h"

/* ============================================================================
 * AFI / SAFI 常量
 * ========================================================================== */

/** @brief 地址族标识符（RFC 1700） */
#define BGP_AFI_IPV4 1   /**< IPv4 */
#define BGP_AFI_IPV6 2   /**< IPv6 */
#define BGP_AFI_L2VPN 25 /**< L2VPN（EVPN） */

/** @brief 子地址族标识符 */
#define BGP_SAFI_UNICAST 1        /**< 单播 */
#define BGP_SAFI_LABELED 4        /**< MPLS 标签单播 */
#define BGP_SAFI_EVPN 70          /**< EVPN（L2VPN） */
#define BGP_SAFI_VPN_UNICAST 128  /**< VPN 单播（MPLS L3VPN） */
#define BGP_SAFI_FLOWSPEC 133     /**< FlowSpec */
#define BGP_SAFI_VPN_FLOWSPEC 134 /**< VPN FlowSpec */

/* ============================================================================
 * 基础类型
 * ========================================================================== */

/**
 * @brief Route Distinguisher（8 字节，RFC 4364）
 */
typedef struct bgp_rd
{
    uint8_t bytes[8]; /**< 原始字节，大端 */
} bgp_rd_t;

/**
 * @brief Ethernet Segment Identifier（10 字节，RFC 7432）
 */
typedef struct bgp_esi
{
    uint8_t bytes[10]; /**< 原始字节 */
} bgp_esi_t;

/**
 * @brief 从 3 字节 label stack entry 解码 MPLS 标签值（高 20 位）
 * @param b 3 字节指针
 * @return 标签值（0-1048575）
 */
static inline uint32_t bgp_label_decode(const uint8_t *b)
{
    return ((uint32_t)b[0] << 12) | ((uint32_t)b[1] << 4) | ((uint32_t)b[2] >> 4);
}

/* ============================================================================
 * BGP 路径属性
 * ========================================================================== */

/** AS_PATH 字符串最大长度 */
#define BGP_ATTR_AS_PATH_MAX 512

/** Community 字符串最大长度 */
#define BGP_ATTR_COMMUNITY_MAX 1024

/**
 * @brief BGP ORIGIN 值
 */
typedef enum bgp_origin
{
    BGP_ORIGIN_IGP = 0,        /**< 内部（IGP） */
    BGP_ORIGIN_EGP = 1,        /**< 外部（EGP） */
    BGP_ORIGIN_INCOMPLETE = 2, /**< 不完整 */
} bgp_origin_t;

/**
 * @brief BGP 路径属性集合
 *
 * 所有字段均为值语义，无动态分配，可直接 memcpy。
 */
typedef struct bgp_attr
{
    bgp_origin_t origin;                            /**< ORIGIN */
    char as_path[BGP_ATTR_AS_PATH_MAX];             /**< AS_PATH 字符串，如 "65001 65002 {65003}" */
    uint32_t local_pref;                            /**< LOCAL_PREF（has_local_pref=true 时有效） */
    uint32_t med;                                   /**< MULTI_EXIT_DISC（has_med=true 时有效） */
    bool has_local_pref;                            /**< LOCAL_PREF 是否存在 */
    bool has_med;                                   /**< MED 是否存在 */
    bool atomic_aggregate;                          /**< ATOMIC_AGGREGATE 标志 */
    char aggregator[48];                            /**< AGGREGATOR，格式 "AS:IP" */
    char communities[BGP_ATTR_COMMUNITY_MAX];       /**< COMMUNITY，空格分隔 "ASN:VAL ..." */
    char ext_communities[BGP_ATTR_COMMUNITY_MAX];   /**< EXT_COMMUNITY，空格分隔 */
    char large_communities[BGP_ATTR_COMMUNITY_MAX]; /**< LARGE_COMMUNITY "GADM:LD1:LD2 ..." */
    net_addr_t originator_id;                       /**< ORIGINATOR_ID（4 字节 IPv4） */
    bool has_originator_id;                         /**< ORIGINATOR_ID 是否存在 */
} bgp_attr_t;

/* ============================================================================
 * BGP Nexthop
 * ========================================================================== */

/**
 * @brief BGP 下一跳（来自 NEXT_HOP 属性或 MP_REACH nexthop 字段）
 */
typedef struct bgp_nexthop
{
    net_addr_t global;     /**< 主 nexthop（IPv4 或 IPv6 全局地址） */
    net_addr_t link_local; /**< IPv6 link-local nexthop（可选） */
    bool has_link_local;   /**< link_local 是否有效 */
} bgp_nexthop_t;

/* ============================================================================
 * NLRI 条目
 * ========================================================================== */

/**
 * @brief NLRI 条目类型
 */
typedef enum bgp_nlri_type
{
    BGP_NLRI_PREFIX = 0,   /**< IP 前缀（unicast/VPN/labeled） */
    BGP_NLRI_EVPN = 1,     /**< L2VPN EVPN 路由 */
    BGP_NLRI_FLOWSPEC = 2, /**< FlowSpec 过滤规则 */
    BGP_NLRI_OPAQUE = 3,   /**< 未知/未注册，保存原始字节 */
} bgp_nlri_type_t;

/**
 * @brief IP 前缀 NLRI（unicast / VPN / MPLS labeled）
 */
typedef struct bgp_nlri_prefix
{
    net_prefix_t prefix; /**< IP 地址 + 前缀长度 */
    bgp_rd_t rd;         /**< Route Distinguisher（VPN 时有效） */
    uint32_t label;      /**< MPLS 标签（0 = 无效） */
    bool has_rd;         /**< rd 是否有效 */
    bool has_label;      /**< label 是否有效 */
} bgp_nlri_prefix_t;

/**
 * @brief EVPN 路由条目（RFC 7432）
 */
typedef struct bgp_nlri_evpn
{
    uint8_t route_type; /**< 1=A-D, 2=MAC/IP, 3=IMET, 4=ES, 5=IP Prefix */
    bgp_rd_t rd;
    bgp_esi_t esi;
    uint32_t eth_tag; /**< Ethernet Tag ID */
    uint8_t mac[6];   /**< MAC 地址（type 2 有效） */
    bool has_mac;
    net_addr_t ip; /**< IP 地址（type 2/3/4 有效） */
    bool has_ip;
    net_prefix_t ip_prefix; /**< IP 前缀（type 5 有效） */
    net_addr_t gw_ip;       /**< Gateway IP（type 5 有效） */
    uint32_t label1;
    uint32_t label2;
    bool has_label2;
    uint8_t raw[512]; /**< 完整原始字节（含 route_type + length） */
    uint16_t raw_len;
} bgp_nlri_evpn_t;

/** FlowSpec 组件类型码（RFC 5575） */
#define BGP_FS_TYPE_DST_PREFIX 1 /**< 目的前缀 */
#define BGP_FS_TYPE_SRC_PREFIX 2 /**< 源前缀 */
#define BGP_FS_TYPE_PROTO 3      /**< IP 协议号 */
#define BGP_FS_TYPE_PORT 4       /**< 端口 */
#define BGP_FS_TYPE_DST_PORT 5   /**< 目的端口 */
#define BGP_FS_TYPE_SRC_PORT 6   /**< 源端口 */
#define BGP_FS_TYPE_ICMP_TYPE 7  /**< ICMP type */
#define BGP_FS_TYPE_ICMP_CODE 8  /**< ICMP code */
#define BGP_FS_TYPE_TCP_FLAGS 9  /**< TCP flags */
#define BGP_FS_TYPE_PKT_LEN 10   /**< 报文长度 */
#define BGP_FS_TYPE_DSCP 11      /**< DSCP */
#define BGP_FS_TYPE_FRAGMENT 12  /**< Fragment 标志 */

/** FlowSpec 单个组件 */
#define BGP_FS_COMP_DATA_MAX 64

/**
 * @brief FlowSpec NLRI 组件
 */
typedef struct bgp_fs_component
{
    uint8_t type;                       /**< 组件类型码 */
    uint8_t data[BGP_FS_COMP_DATA_MAX]; /**< 原始字节 */
    uint8_t data_len;
    char str[128]; /**< 可读字符串，如 "dport=80" */
} bgp_fs_component_t;

/** FlowSpec 最大组件数 */
#define BGP_FS_MAX_COMPONENTS 16

/**
 * @brief FlowSpec NLRI
 */
typedef struct bgp_nlri_flowspec
{
    bgp_fs_component_t components[BGP_FS_MAX_COMPONENTS]; /**< 组件列表 */
    uint8_t count;                                        /**< 组件数量 */
    bgp_rd_t rd;                                          /**< VPN FlowSpec 时有效 */
    bool has_rd;
} bgp_nlri_flowspec_t;

/** NLRI 条目 key 最大长度 */
#define BGP_NLRI_KEY_MAX 256

/**
 * @brief NLRI 条目（统一表示，覆盖所有 AFI/SAFI）
 */
typedef struct bgp_nlri_entry
{
    uint16_t afi;
    uint8_t safi;
    bgp_nlri_type_t type;
    union
    {
        bgp_nlri_prefix_t prefix;     /**< unicast/VPN/labeled */
        bgp_nlri_evpn_t evpn;         /**< EVPN */
        bgp_nlri_flowspec_t flowspec; /**< FlowSpec */
        struct
        {
            uint8_t data[512];
            uint16_t len;
        } opaque; /**< 原始字节（未知 AF） */
    };
    char key[BGP_NLRI_KEY_MAX]; /**< 唯一字符串键（哈希/显示用），由 entry_to_str 填充 */
} bgp_nlri_entry_t;

/* ============================================================================
 * UPDATE 解析结果
 * ========================================================================== */

/**
 * @brief BGP UPDATE 报文解析结果
 *
 * reach/unreach 为堆分配数组，调用者须通过 bgp_update_result_free() 释放。
 */
typedef struct bgp_update_result
{
    bgp_attr_t attr;           /**< 路径属性（对所有 reach 路由共用） */
    bgp_nexthop_t nexthop;     /**< 下一跳 */
    bgp_nlri_entry_t *reach;   /**< 新增/更新 NLRI 数组（堆分配） */
    uint32_t reach_len;        /**< reach 数组元素数 */
    bgp_nlri_entry_t *unreach; /**< 撤销 NLRI 数组（堆分配） */
    uint32_t unreach_len;      /**< unreach 数组元素数 */
    uint16_t afi;              /**< 主要地址族（MP_REACH 时填充） */
    uint8_t safi;              /**< 主要子地址族 */
} bgp_update_result_t;

#endif /* BGP_ROUTE_H */
