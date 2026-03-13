/**
 * @file   bgp_msg.h
 * @brief  BGP 控制报文结构定义：OPEN、NOTIFICATION、PDU header
 * @author jhb
 * @date   2026/03/13
 */
#ifndef BGP_MSG_H
#define BGP_MSG_H

#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * BGP 报文类型
 * ========================================================================== */

/** @brief BGP 报文类型码（RFC 4271） */
#define BGP_MSG_TYPE_OPEN 1         /**< OPEN 报文 */
#define BGP_MSG_TYPE_UPDATE 2       /**< UPDATE 报文 */
#define BGP_MSG_TYPE_NOTIFICATION 3 /**< NOTIFICATION 报文 */
#define BGP_MSG_TYPE_KEEPALIVE 4    /**< KEEPALIVE 报文 */

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

#endif /* BGP_MSG_H */
