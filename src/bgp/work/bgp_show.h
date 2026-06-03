/**
 * @file   bgp_show.h
 * @brief  BGP show 命令公共内部接口（bgp_show.c 与 bgp_show_route.c 共享）
 * @author jhb
 * @date   2026/06/01
 */
#ifndef BGP_SHOW_H
#define BGP_SHOW_H

#include <glib.h>

#include "bgp.h"
#include "bgp_vrf.h"
#include "cli.h"
#include "dev.h"
#include "vrf.h"

/**
 * @brief show 命令上下文（AF + VRF 名）
 */
typedef struct bgp_cli_ctx
{
    char vrf_name[VRF_NAME_MAX_LEN]; /**< VRF 名（默认 public） */
    bgp_afi_t afi;                   /**< 地址族 */
    bgp_safi_t safi;                 /**< 子地址族 */
} bgp_cli_ctx_t;

/** 发送单段 show 文本响应 */
void bgp_show_send_cli_response(dev_ipc_message_t *msg, const char *text);

/** 发送分片 show 响应（GString 由本函数接管） */
int bgp_work_send_chunked_response(dev_ipc_message_t *msg, GString *full_text);

/** 返回默认 show 上下文（public VRF / ipv4-unicast） */
bgp_cli_ctx_t bgp_cli_ctx_default(void);

/** 从 CLI TLV 上下文条目解析 AF / VRF 名到 ctx */
void bgp_cli_ctx_parse(bgp_cli_ctx_t *ctx, cli_tlv_entry_t *entry);

/** 按 ctx 中的 VRF 名查找 bgp_vrf_t（未找到返回 NULL） */
bgp_vrf_t *bgp_show_lookup_vrf(const bgp_cli_ctx_t *ctx);

/** AF 文本标识（如 "vpnv4"） */
const char *bgp_af_str(bgp_afi_t afi, bgp_safi_t safi);

/** ORIGIN 文本 */
const char *bgp_origin_str(bgp_origin_t origin);

/** 微秒时间戳格式化为可读字符串 */
void bgp_fmt_time_usec(gint64 usec, char *buf, size_t sz);

#endif /* BGP_SHOW_H */
