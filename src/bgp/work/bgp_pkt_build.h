/**
 * @file   bgp_pkt_build.h
 * @brief  BGP UPDATE 报文 AF 编码器注册接口（内部头文件）
 * @author jhb
 * @date   2026/03/15
 */
#ifndef BGP_PKT_BUILD_H
#define BGP_PKT_BUILD_H

#include <stdint.h>

#include "bgp.h"

/* 前向声明：回调函数参数使用指针，无需完整定义 */
typedef struct bgp_conn bgp_conn_t;

// ============================================================================
// 路径属性公共常量
// ============================================================================

/** 路径属性标志位 */
#define BGP_PA_FLAG_OPTIONAL 0x80u   /**< Optional bit */
#define BGP_PA_FLAG_TRANSITIVE 0x40u /**< Transitive bit */
#define BGP_PA_FLAG_EXT_LEN 0x10u    /**< Extended Length bit */

/** 路径属性类型码（RFC 4271 / RFC 4760） */
#define BGP_PA_TYPE_ORIGIN 1u         /**< ORIGIN */
#define BGP_PA_TYPE_AS_PATH 2u        /**< AS_PATH */
#define BGP_PA_TYPE_NEXT_HOP 3u       /**< NEXT_HOP */
#define BGP_PA_TYPE_MED 4u            /**< MULTI_EXIT_DISC */
#define BGP_PA_TYPE_LOCAL_PREF 5u     /**< LOCAL_PREF */
#define BGP_PA_TYPE_COMMUNITY 8u      /**< COMMUNITY */
#define BGP_PA_TYPE_ORIGINATOR_ID 9u  /**< ORIGINATOR_ID（RFC 4456） */
#define BGP_PA_TYPE_CLUSTER_LIST 10u  /**< CLUSTER_LIST（RFC 4456） */
#define BGP_PA_TYPE_MP_REACH 14u      /**< MP_REACH_NLRI（RFC 4760） */
#define BGP_PA_TYPE_MP_UNREACH 15u    /**< MP_UNREACH_NLRI（RFC 4760） */
#define BGP_PA_TYPE_EXT_COMMUNITY 16u /**< EXTENDED_COMMUNITIES（RFC 4360） */

// ============================================================================
// AF 编码器描述符
// ============================================================================

/**
 * @brief BGP UPDATE 报文 AF 相关编码器描述符
 *
 * AF 差异化编码；公共属性（ORIGIN / AS_PATH / LOCAL_PREF / MED / COMMUNITY）
 * 由 bgp_pkt.c 统一处理，不经由此接口。
 *
 * 所有回调函数：
 *   - 返回实际写入的字节数（>= 0），-1 表示缓冲区空间不足
 *   - 返回 0 表示该字段对当前 AF 为空（如 IPv4 撤销无额外 PA）
 */
typedef struct bgp_pkt_af_enc
{
    uint16_t afi; /**< 地址族（BGP_AFI_IPV4 / BGP_AFI_IPV6 / ...） */
    uint8_t safi; /**< 子地址族（BGP_SAFI_UNICAST / ...） */

    /**
     * @brief 编码宣告报文的 AF 相关路径属性
     *        IPv4: NEXT_HOP；IPv6: MP_REACH_NLRI（含 nexthop + 前缀）
     */
    int (*encode_reach_pa)(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_nexthop_t *nexthop);

    /**
     * @brief 编码宣告报文的 NLRI 字段内容
     *        IPv4: 前缀（prefix_len + 有效字节）；IPv6: 返回 0（已在 MP_REACH 中携带）
     */
    int (*encode_reach_nlri)(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_nexthop_t *nexthop);

    /**
     * @brief 编码撤销报文的 Withdrawn Routes 字段内容
     *        IPv4（IPv4 peer）: 前缀；IPv4（IPv6 peer）: 返回 0（RFC 8950）
     *        IPv6: 返回 0（前缀通过 MP_UNREACH_NLRI 携带）
     */
    int (*encode_unreach_wd)(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn);

    /**
     * @brief 编码撤销报文的 AF 相关路径属性
     *        IPv4（IPv6 peer）: MP_UNREACH_NLRI（RFC 8950）；IPv4（IPv4 peer）: 返回 0
     *        IPv6: MP_UNREACH_NLRI
     */
    int (*encode_unreach_pa)(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn);

    /**
     * @brief 打包版：编码宣告路径属性（多 NLRI 共享属性）
     *
     * IPv4 + IPv4 nh：写入传统 NEXT_HOP 属性（7 字节），*out_packed = nlri_count
     * IPv4 + IPv6 nh（RFC 8950）：写入 MP_REACH_NLRI（AFI+SAFI+NHLen+NH+SNPA + 逐条 IPv4 前缀），
     *                              *out_packed = 实际嵌入的 NLRI 数
     * IPv6：写入 MP_REACH_NLRI（含 nh + 逐条 IPv6 前缀），*out_packed = 实际嵌入数
     *
     * @return 写入字节数，-1=连属性头部都放不下（需空缓冲重试）
     */
    int (*encode_reach_pa_packed)(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                  const bgp_nexthop_t *nexthop, int *out_packed);

    /**
     * @brief 打包版：编码宣告 NLRI 段
     *
     * IPv4 + IPv4 nh：连续打包多条前缀，*out_packed = 实际打包数
     * IPv4 + IPv6 nh / IPv6：返回 0，*out_packed = nlri_count（NLRI 已在 MP_REACH 中）
     */
    int (*encode_reach_nlri_packed)(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                    int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed);

    /**
     * @brief 打包版：编码撤销 Withdrawn Routes 段
     *
     * IPv4（IPv4 peer）：连续打包前缀，*out_packed = 实际打包数
     * IPv4（IPv6 peer）/ IPv6：返回 0，*out_packed = nlri_count
     */
    int (*encode_unreach_wd_packed)(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                    int nlri_count, const bgp_conn_t *conn, int *out_packed);

    /**
     * @brief 打包版：编码撤销 AF 路径属性
     *
     * IPv4（IPv6 peer）/ IPv6：MP_UNREACH_NLRI（AFI+SAFI + 逐条前缀），*out_packed = 嵌入数
     * IPv4（IPv4 peer）：返回 0，*out_packed = nlri_count
     */
    int (*encode_unreach_pa_packed)(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                    int nlri_count, const bgp_conn_t *conn, int *out_packed);
} bgp_pkt_af_enc_t;

// ============================================================================
// 注册表 API
// ============================================================================

/**
 * @brief 注册 AF 编码器（描述符生命周期须长于库）
 * @return 0=成功, -1=失败（表已满或 enc 为 NULL）
 */
int bgp_pkt_af_enc_register(const bgp_pkt_af_enc_t *enc);

/**
 * @brief 按 AFI/SAFI 查找已注册编码器
 * @return 编码器指针，NULL=未注册
 */
const bgp_pkt_af_enc_t *bgp_pkt_af_enc_find(uint16_t afi, uint8_t safi);

// ============================================================================
// 公共编码辅助函数
// ============================================================================

/**
 * @brief 编码 IP 前缀（prefix_len + 有效字节），供 AF 编码器复用
 * @param buf      输出缓冲区
 * @param buf_size 可用空间
 * @param pfx      前缀结构
 * @return 写入字节数，-1=空间不足
 */
int bgp_pkt_encode_prefix(uint8_t *buf, int buf_size, const net_prefix_t *pfx);

// ============================================================================
// 初始化
// ============================================================================

/**
 * @brief 注册所有内置 AF 编码器（在 bgp_module_init 中调用一次，幂等）
 */
void bgp_pkt_build_init(void);

#endif /* BGP_PKT_BUILD_H */
