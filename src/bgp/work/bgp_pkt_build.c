/**
 * @file   bgp_pkt_build.c
 * @brief  BGP UPDATE 报文 AF 编码器注册表 + 公共辅助函数
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_pkt_build.h"

#include <string.h>

#include "log.h"

/** 注册表最大容量 */
#define BGP_PKT_AF_ENC_MAX 16

/** 全局注册表 */
static const bgp_pkt_af_enc_t *g_enc_table[BGP_PKT_AF_ENC_MAX];
static int g_enc_count = 0;

// ============================================================================
// 注册表 API
// ============================================================================

int bgp_pkt_af_enc_register(const bgp_pkt_af_enc_t *enc)
{
    if (!enc || g_enc_count >= BGP_PKT_AF_ENC_MAX)
    {
        LOG_ERROR("BGP: pkt_af_enc_register 失败（enc=%p count=%d）", (void *)enc, g_enc_count);
        return -1;
    }
    g_enc_table[g_enc_count++] = enc;
    LOG_DEBUG("BGP: 已注册 UPDATE 编码器 AFI=%u SAFI=%u", enc->afi, enc->safi);
    return 0;
}

const bgp_pkt_af_enc_t *bgp_pkt_af_enc_find(uint16_t afi, uint8_t safi)
{
    for (int i = 0; i < g_enc_count; i++)
    {
        if (g_enc_table[i]->afi == afi && g_enc_table[i]->safi == safi)
        {
            return g_enc_table[i];
        }
    }
    return NULL;
}

// ============================================================================
// 公共编码辅助函数
// ============================================================================

int bgp_pkt_encode_prefix(uint8_t *buf, int buf_size, const net_prefix_t *pfx)
{
    uint8_t plen = pfx->prefix_len;
    int nbytes = (plen + 7) / 8;
    if (buf_size < 1 + nbytes)
    {
        return -1;
    }
    buf[0] = plen;
    if (pfx->addr.family == AF_INET)
    {
        memcpy(buf + 1, &pfx->addr.u.v4, nbytes);
    }
    else if (pfx->addr.family == AF_INET6)
    {
        memcpy(buf + 1, &pfx->addr.u.v6, nbytes);
    }
    else
    {
        memset(buf + 1, 0, nbytes);
    }
    return 1 + nbytes;
}

// ============================================================================
// 初始化：注册内置 AF 编码器
// ============================================================================

/* 内置编码器注册函数（各实现文件提供） */
void bgp_pkt_build_ipv4uc_register(void);
void bgp_pkt_build_ipv6uc_register(void);
void bgp_pkt_build_ipv4qp_register(void);
void bgp_pkt_build_ipv6qp_register(void);

void bgp_pkt_build_init(void)
{
    if (g_enc_count > 0)
    {
        return; /* 已初始化，幂等 */
    }
    bgp_pkt_build_ipv4uc_register();
    bgp_pkt_build_ipv6uc_register();
    bgp_pkt_build_ipv4qp_register();
    bgp_pkt_build_ipv6qp_register();
}
