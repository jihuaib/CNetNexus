/**
 * @file   bgp.h
 * @brief  BGP 公共统一头文件（合并 bgp_msg.h / bgp_route.h / bgp_parse.h）
 * @author jhb
 * @date   2026/03/15
 */
#ifndef BGP_H
#define BGP_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "net_addr.h"

/* ============================================================================
 * BGP 报文类型
 * ========================================================================== */

/** @brief BGP 报文类型码（RFC 4271） */
#define BGP_MSG_TYPE_OPEN 1          /**< OPEN 报文 */
#define BGP_MSG_TYPE_UPDATE 2        /**< UPDATE 报文 */
#define BGP_MSG_TYPE_NOTIFICATION 3  /**< NOTIFICATION 报文 */
#define BGP_MSG_TYPE_KEEPALIVE 4     /**< KEEPALIVE 报文 */
#define BGP_MSG_TYPE_ROUTE_REFRESH 5 /**< ROUTE-REFRESH 报文（RFC 2918） */

/** BGP header 长度（Marker 16B + Length 2B + Type 1B） */
#define BGP_HEADER_LEN 19

/* ============================================================================
 * BGP PDU header 解析结果
 * ========================================================================== */

/**
 * @brief BGP PDU header 解析结果（用于 BMP Route Monitoring 等场景）
 */
typedef struct bgp_pdu_info
{
    uint8_t msg_type;    /**< BGP_MSG_TYPE_* */
    uint16_t msg_len;    /**< 报文总长度（含 header） */
    const uint8_t *body; /**< 报文体起始指针（header 之后） */
    uint16_t body_len;   /**< 报文体长度 = msg_len - BGP_HEADER_LEN */
} bgp_pdu_info_t;

/* ============================================================================
 * BGP OPEN 能力码
 * ========================================================================== */

/** @brief BGP 能力码（RFC 5492） */
#define BGP_CAP_MP_EXTENSIONS 1     /**< 多协议扩展（RFC 4760） */
#define BGP_CAP_ROUTE_REFRESH 2     /**< Route Refresh（RFC 2918） */
#define BGP_CAP_EXT_NEXTHOP 5       /**< 扩展下一跳编码（RFC 8950） */
#define BGP_CAP_EXT_MESSAGE 6       /**< 扩展消息（RFC 8654） */
#define BGP_CAP_BGP_ROLE 9          /**< BGP Role（RFC 9234） */
#define BGP_CAP_GRACEFUL_RESTART 64 /**< Graceful Restart（RFC 4724） */
#define BGP_CAP_AS4 65              /**< 4 字节 AS 号（RFC 6793） */
#define BGP_CAP_ADD_PATH 69         /**< ADD-PATH（RFC 7911） */
#define BGP_CAP_ENHANCED_RR 70      /**< Enhanced Route Refresh（RFC 7313） */
#define BGP_CAP_FQDN 73             /**< FQDN（draft-walton-bgp-hostname-capability） */

/** ADD-PATH 每条记录的 send/receive 标志位 */
#define BGP_ADDPATH_RECEIVE (1U << 0) /**< 本端可接收对端的多路径 */
#define BGP_ADDPATH_SEND (1U << 1)    /**< 本端可向对端发送多路径 */

/** OPEN 中最多携带的 Extended Next Hop 条数 */
#define BGP_OPEN_MAX_EXT_NH 16

/**
 * @brief Extended Next Hop 能力条目（RFC 8950）
 */
typedef struct bgp_ext_nh_entry
{
    uint16_t nlri_afi;  /**< NLRI 地址族（如 IPv4=1） */
    uint16_t nlri_safi; /**< NLRI 子地址族（如 unicast=1） */
    uint16_t nh_afi;    /**< Nexthop 地址族（如 IPv6=2） */
} bgp_ext_nh_entry_t;

/** OPEN 中最多携带的 MP 能力条数 */
#define BGP_OPEN_MAX_MP_CAPS 32
/** OPEN 中最多携带的 ADD-PATH 条数 */
#define BGP_OPEN_MAX_ADDPATH 16
/** OPEN 中 Graceful Restart 最多携带的 AF 条数 */
#define BGP_OPEN_MAX_GR_AFS 16

/**
 * @brief Graceful Restart 单个 AF 条目
 */
typedef struct bgp_gr_af
{
    uint16_t afi;
    uint8_t safi;
    bool forwarding_state; /**< F bit：是否保留转发状态 */
} bgp_gr_af_t;

/**
 * @brief Graceful Restart 能力
 */
typedef struct bgp_cap_gr
{
    bool restart_flag;      /**< Restart State bit */
    bool notification_flag; /**< Notification bit（RFC 8538） */
    uint16_t restart_time;  /**< Restart Time（秒） */
    bgp_gr_af_t afs[BGP_OPEN_MAX_GR_AFS];
    uint8_t af_count;
} bgp_cap_gr_t;

/**
 * @brief ADD-PATH 单个 AF 条目
 */
typedef struct bgp_addpath_af
{
    uint16_t afi;
    uint8_t safi;
    uint8_t flags; /**< BGP_ADDPATH_RECEIVE / BGP_ADDPATH_SEND */
} bgp_addpath_af_t;

/**
 * @brief BGP OPEN 报文解析结果
 */
typedef struct bgp_open_msg
{
    uint8_t version;    /**< 版本，应为 4 */
    uint16_t my_as;     /**< 2 字节 AS（AS4 时可能为 AS_TRANS=23456） */
    uint16_t hold_time; /**< Hold Time（秒） */
    char bgp_id[16];    /**< BGP Identifier（点分十进制） */

    /* ---- 能力标志 ---- */
    bool cap_route_refresh; /**< Route Refresh（code=2） */
    bool cap_ext_message;   /**< Extended Message（code=6） */
    bool cap_enhanced_rr;   /**< Enhanced Route Refresh（code=70） */
    uint32_t cap_as4;       /**< 4 字节 AS（0=未携带，非零=真实 AS） */
    uint8_t cap_bgp_role;   /**< BGP Role（255=未携带） */

    /* ---- MP Extensions ---- */
    uint16_t mp_afs[BGP_OPEN_MAX_MP_CAPS];  /**< AFI 数组（与 mp_safis 一一对应） */
    uint8_t mp_safis[BGP_OPEN_MAX_MP_CAPS]; /**< SAFI 数组 */
    uint8_t mp_count;                       /**< MP 能力条数 */

    /* ---- Extended Next Hop (RFC 8950) ---- */
    bool cap_ext_nexthop;
    bgp_ext_nh_entry_t ext_nh[BGP_OPEN_MAX_EXT_NH];
    uint8_t ext_nh_count;

    /* ---- Graceful Restart ---- */
    bool cap_graceful_restart;
    bgp_cap_gr_t graceful_restart;

    /* ---- ADD-PATH ---- */
    bool cap_add_path;
    bgp_addpath_af_t add_path[BGP_OPEN_MAX_ADDPATH];
    uint8_t add_path_count;

    /* ---- FQDN ---- */
    char hostname[64]; /**< 设备主机名（空字符串=未携带） */
    char domain[128];  /**< 设备域名（空字符串=未携带） */
} bgp_open_msg_t;

/* ============================================================================
 * BGP NOTIFICATION 错误码
 * ========================================================================== */

/** @brief NOTIFICATION Error Code */
#define BGP_ERR_MSG_HDR 1    /**< Message Header Error */
#define BGP_ERR_OPEN 2       /**< OPEN Message Error */
#define BGP_ERR_UPDATE 3     /**< UPDATE Message Error */
#define BGP_ERR_HOLD_TIMER 4 /**< Hold Timer Expired */
#define BGP_ERR_FSM 5        /**< FSM Error */
#define BGP_ERR_CEASE 6      /**< Cease */

/** @brief Error Code 2（OPEN）Sub-code */
#define BGP_OPEN_ERR_UNSUPPORTED_VERSION 1
#define BGP_OPEN_ERR_BAD_PEER_AS 2
#define BGP_OPEN_ERR_BAD_BGP_ID 3
#define BGP_OPEN_ERR_UNSUPPORTED_OPT 4
#define BGP_OPEN_ERR_UNACCEPTABLE_HOLD 6
#define BGP_OPEN_ERR_UNSUPPORTED_CAP 7

/** @brief Error Code 6（Cease）Sub-code */
#define BGP_CEASE_MAX_PREFIXES 1
#define BGP_CEASE_ADMIN_SHUTDOWN 2
#define BGP_CEASE_PEER_DECONFIG 3
#define BGP_CEASE_ADMIN_RESET 4
#define BGP_CEASE_CONN_REJECTED 5
#define BGP_CEASE_OTHER_CONFIG 6
#define BGP_CEASE_CONN_COLLISION 7
#define BGP_CEASE_OUT_OF_RESOURCES 8
#define BGP_CEASE_HARD_RESET 9 /**< RFC 8538 */

/** NOTIFICATION 附带数据最大字节数 */
#define BGP_NOTIF_DATA_MAX 256

/**
 * @brief BGP NOTIFICATION 报文解析结果
 */
typedef struct bgp_notif_msg
{
    uint8_t error_code;               /**< Error Code */
    uint8_t error_subcode;            /**< Error Subcode */
    uint8_t data[BGP_NOTIF_DATA_MAX]; /**< 附带数据（截断至 BGP_NOTIF_DATA_MAX） */
    uint16_t data_len;                /**< 实际数据长度 */
    char error_str[128];              /**< 可读错误描述，如 "Cease/Admin-Shutdown" */
} bgp_notif_msg_t;

/* ============================================================================
 * AFI / SAFI 常量
 * ========================================================================== */

/** 地址族标识符（RFC 1700） */
typedef enum bgp_afi
{
    BGP_AFI_IPV4 = 1,  /**< IPv4 */
    BGP_AFI_IPV6 = 2,  /**< IPv6 */
    BGP_AFI_L2VPN = 25 /**< L2VPN（EVPN） */
} bgp_afi_t;

/** 子地址族标识符 */
typedef enum bgp_safi
{
    BGP_SAFI_UNICAST = 1,        /**< 单播 */
    BGP_SAFI_LABELED = 4,        /**< MPLS 标签单播 */
    BGP_SAFI_EVPN = 70,          /**< EVPN（L2VPN） */
    BGP_SAFI_VPN_UNICAST = 128,  /**< VPN 单播（MPLS L3VPN） */
    BGP_SAFI_FLOWSPEC = 133,     /**< FlowSpec */
    BGP_SAFI_VPN_FLOWSPEC = 134, /**< VPN FlowSpec */
    BGP_SAFI_QP = 253            /**< QP */
} bgp_safi_t;

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

/** Extended Community 原始报文 buffer 最大长度（8 字节对齐） */
#define BGP_ATTR_EXT_COMMUNITY_MAX 1024

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
    bgp_origin_t origin;                                 /**< ORIGIN */
    char as_path[BGP_ATTR_AS_PATH_MAX];                  /**< AS_PATH 字符串，如 "65001 65002 {65003}" */
    uint32_t local_pref;                                 /**< LOCAL_PREF（has_local_pref=true 时有效） */
    uint32_t med;                                        /**< MULTI_EXIT_DISC（has_med=true 时有效） */
    bool has_local_pref;                                 /**< LOCAL_PREF 是否存在 */
    bool has_med;                                        /**< MED 是否存在 */
    bool atomic_aggregate;                               /**< ATOMIC_AGGREGATE 标志 */
    char aggregator[48];                                 /**< AGGREGATOR，格式 "AS:IP" */
    char communities[BGP_ATTR_COMMUNITY_MAX];            /**< COMMUNITY，空格分隔 "ASN:VAL ..." */
    uint8_t ext_communities[BGP_ATTR_EXT_COMMUNITY_MAX]; /**< EXT_COMMUNITY 原始 8 字节条目 buffer */
    uint16_t ext_communities_len;                        /**< EXT_COMMUNITY 原始 buffer 长度 */
    char large_communities[BGP_ATTR_COMMUNITY_MAX];      /**< LARGE_COMMUNITY "GADM:LD1:LD2 ..." */
    net_addr_t originator_id;                            /**< ORIGINATOR_ID（4 字节 IPv4） */
    bool has_originator_id;                              /**< ORIGINATOR_ID 是否存在 */
    net_addr_t cluster_list[16];                         /**< CLUSTER_LIST（IPv4 cluster-id 数组，RFC 4456） */
    uint8_t cluster_list_len;                            /**< CLUSTER_LIST 元素数（0 表示无） */
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
    BGP_NLRI_QP = 4,       /**< QP（SAFI=253）变长 TLV：dqpn + 前缀 */
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

/** QP TLV 类型：Destination QP Number */
#define BGP_QP_TLV_DQPN 1
/** QP TLV 类型：IP 前缀 */
#define BGP_QP_TLV_PREFIX 2

/**
 * @brief QP NLRI（SAFI=253）
 *
 * 线上格式：长度(1) | TLV1=dqpn(type 1 + bit-len + dqpn 1-3B) | TLV2=prefix(type 2 + mask + prefix)
 */
typedef struct bgp_nlri_qp
{
    uint32_t dqpn;       /**< Destination QP Number（主机字节序，<= 0xFFFFFF） */
    uint8_t dqpn_len;    /**< dqpn 编码 bit 数 1~24 */
    net_prefix_t prefix; /**< IP 前缀（v4/v6 由 AFI 决定） */
} bgp_nlri_qp_t;

/** NLRI 条目字符串最大长度（显示/日志用） */
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
        bgp_nlri_qp_t qp;             /**< QP（SAFI=253） */
        struct
        {
            uint8_t data[512];
            uint16_t len;
        } opaque; /**< 原始字节（未知 AF） */
    };
} bgp_nlri_entry_t;

/**
 * @brief 比较两个 NLRI（字段级二进制比较）
 *
 * 返回值语义与 strcmp 相同：<0 / 0 / >0。
 */
int bgp_nlri_cmp(const bgp_nlri_entry_t *a, const bgp_nlri_entry_t *b);

/**
 * @brief 判断两个 NLRI 是否相等
 */
bool bgp_nlri_equal(const bgp_nlri_entry_t *a, const bgp_nlri_entry_t *b);

/**
 * @brief 将 NLRI 格式化为可读字符串
 *
 * 仅调用已注册 AFI/SAFI 的 entry_to_str 回调；未注册时输出空字符串。
 */
void bgp_nlri_to_str(const bgp_nlri_entry_t *entry, char *buf, size_t sz);

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

/* ============================================================================
 * 解析标志
 * ========================================================================== */

/** 使用 4 字节 AS 号（RFC 6793），默认开启 */
#define BGP_PARSE_FLAG_AS4 (1u << 0)

/** 已协商 Extended Next Hop（RFC 8950），允许 IPv4 NLRI 携带 IPv6 nexthop */
#define BGP_PARSE_FLAG_EXT_NEXTHOP (1u << 1)

/* ============================================================================
 * AFI/SAFI 处理器注册接口
 * ========================================================================== */

/**
 * @brief AFI/SAFI NLRI 处理器描述符
 *
 * 各 AF 模块（bgp_parse_ipv4uc.c 等）实现并通过 bgp_af_parser_register()
 * 注册。外部模块也可注册自定义处理器以扩展新的 AFI/SAFI 支持。
 */
typedef struct bgp_af_parser
{
    uint16_t afi; /**< 地址族（1=IPv4, 2=IPv6, 25=L2VPN, ...） */
    uint8_t safi; /**< 子地址族（1=unicast, 4=labeled, 70=EVPN, 128=VPN, 133=FlowSpec） */

    /**
     * @brief 解析 MP_REACH NLRI 字节流，输出 NLRI 条目数组
     * @param data    NLRI 字节（SNPA 已跳过，直接为前缀数据）
     * @param len     字节长度
     * @param out     输出条目数组（内部 malloc，调用者 free）
     * @param out_len 输出条目数量
     * @return 0=成功, -1=解析错误
     */
    int (*parse_reach)(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len);

    /**
     * @brief 解析 MP_UNREACH NLRI 字节流，输出撤销条目数组
     * @param data    NLRI 字节
     * @param len     字节长度
     * @param out     输出条目数组（内部 malloc，调用者 free）
     * @param out_len 输出条目数量
     * @return 0=成功, -1=解析错误
     */
    int (*parse_unreach)(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len);

    /**
     * @brief 解析 MP_REACH nexthop 字节（格式因 AFI 而异）
     * @param nh_data nexthop 字节
     * @param nh_len  字节长度
     * @param nexthop 输出 nexthop
     * @return 0=成功, -1=格式错误
     */
    int (*parse_nexthop)(const uint8_t *nh_data, uint8_t nh_len, bgp_nexthop_t *nexthop);

    /**
     * @brief 将 entry 格式化为字符串
     * @param entry 输入 NLRI
     * @param buf   输出缓冲区
     * @param sz    输出缓冲区大小
     */
    void (*entry_to_str)(const bgp_nlri_entry_t *entry, char *buf, size_t sz);
} bgp_af_parser_t;

/**
 * @brief 注册 AFI/SAFI 处理器
 * @param parser 处理器描述符（生命周期须长于库）
 * @return 0=成功, -1=失败
 */
int bgp_af_parser_register(const bgp_af_parser_t *parser);

/**
 * @brief 查找已注册的 AFI/SAFI 处理器
 * @param afi  地址族
 * @param safi 子地址族
 * @return 处理器指针，NULL=未注册
 */
const bgp_af_parser_t *bgp_af_parser_find(uint16_t afi, uint8_t safi);

/* ============================================================================
 * UPDATE 解析主 API
 * ========================================================================== */

/**
 * @brief 解析 BGP UPDATE 报文体（不含 19 字节 BGP header）
 *
 * 调用者须通过 bgp_update_result_free() 释放 *result。
 * 函数返回 -1 时 *result 为 NULL。
 *
 * @param body     报文体起始地址（从 Withdrawn Routes Length 字段开始）
 * @param body_len 报文体长度（字节）
 * @param flags    解析标志（BGP_PARSE_FLAG_*），0 = 默认（AS4 开启）
 * @param result   输出结果（内部分配）
 * @return 0=成功, -1=解析错误
 */
int bgp_update_parse(const uint8_t *body, uint32_t body_len, uint32_t flags, bgp_update_result_t **result);

/**
 * @brief 释放 UPDATE 解析结果（NULL 安全）
 * @param result 待释放结果
 */
void bgp_update_result_free(bgp_update_result_t *result);

/**
 * @brief 初始化 BGP parse 库，注册所有内置 AFI/SAFI 处理器
 *
 * 在首次调用 bgp_update_parse() 之前必须调用一次；多次调用幂等。
 */
void bgp_parse_init(void);

/* ============================================================================
 * BGP PDU Header 解析
 * ========================================================================== */

/**
 * @brief 解析 BGP PDU header（Marker + Length + Type），验证合法性
 *
 * @param data     PDU 起始地址
 * @param data_len 可用字节数
 * @param info     输出解析结果（body 指针指向原始缓冲区，非拷贝）
 * @return 0=成功, -1=数据不足或格式错误
 */
int bgp_pdu_parse_hdr(const uint8_t *data, uint32_t data_len, bgp_pdu_info_t *info);

/* ============================================================================
 * BGP OPEN 解析
 * ========================================================================== */

/**
 * @brief 解析 BGP OPEN 报文体（不含 19 字节 header）
 *
 * @param body     报文体起始地址（version 字节开始）
 * @param body_len 报文体长度
 * @param msg      输出解析结果
 * @return 0=成功, -1=格式错误
 */
int bgp_open_parse(const uint8_t *body, uint16_t body_len, bgp_open_msg_t *msg);

/* ============================================================================
 * BGP NOTIFICATION 解析
 * ========================================================================== */

/**
 * @brief 解析 BGP NOTIFICATION 报文体（不含 19 字节 header）
 *
 * @param body     报文体起始地址（error_code 字节开始）
 * @param body_len 报文体长度
 * @param msg      输出解析结果
 * @return 0=成功（即使格式不完整也尽力解析）, -1=body 为 NULL
 */
int bgp_notif_parse(const uint8_t *body, uint16_t body_len, bgp_notif_msg_t *msg);

/**
 * @brief 返回 NOTIFICATION 错误码的可读字符串
 *
 * @param code    Error Code
 * @param subcode Error Subcode
 * @return 静态字符串，如 "Cease/Admin-Shutdown"
 */
const char *bgp_notif_error_str(uint8_t code, uint8_t subcode);

#endif /* BGP_H */
