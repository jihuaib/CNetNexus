/**
 * @file   bgp_parse.c
 * @brief  BGP UPDATE 报文解析主入口及库初始化
 * @author jhb
 * @date   2026/03/11
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_parse_priv.h"

/* ============================================================================
 * 内部：合并两段 NLRI 数组（调用者负责 free 原数组）
 * ========================================================================== */

static bgp_nlri_entry_t *merge_nlri(bgp_nlri_entry_t *a, uint32_t a_len, bgp_nlri_entry_t *b, uint32_t b_len,
                                    uint32_t *out_len)
{
    *out_len = a_len + b_len;
    if (*out_len == 0)
    {
        return NULL;
    }

    bgp_nlri_entry_t *merged = malloc(*out_len * sizeof(bgp_nlri_entry_t));
    if (!merged)
    {
        *out_len = 0;
        return NULL;
    }

    if (a_len > 0)
    {
        memcpy(merged, a, a_len * sizeof(bgp_nlri_entry_t));
    }
    if (b_len > 0)
    {
        memcpy(merged + a_len, b, b_len * sizeof(bgp_nlri_entry_t));
    }

    return merged;
}

/* ============================================================================
 * 内部：为 IPv4 legacy NLRI 条目补充 nexthop（来自 NEXT_HOP attr）
 * ========================================================================== */

static void fill_afi_safi(bgp_nlri_entry_t *entries, uint32_t count, uint16_t afi, uint8_t safi)
{
    for (uint32_t i = 0; i < count; i++)
    {
        entries[i].afi = afi;
        entries[i].safi = safi;
    }
}

/* ============================================================================
 * bgp_update_parse
 * ========================================================================== */

int bgp_update_parse(const uint8_t *body, uint32_t body_len, uint32_t flags, bgp_update_result_t **result)
{
    *result = NULL;

    if (!body || body_len < 4)
    {
        return -1;
    }

    bgp_update_result_t *r = calloc(1, sizeof(bgp_update_result_t));
    if (!r)
    {
        return -1;
    }

    /* 提前声明所有中间变量，避免 goto 跳过初始化的编译警告 */
    bgp_nlri_entry_t *ipv4_wd = NULL;
    uint32_t ipv4_wd_len = 0;
    bgp_nlri_entry_t *ipv4_reach = NULL;
    uint32_t ipv4_reach_len = 0;
    bgp_nlri_entry_t *mp_reach_entries = NULL;
    uint32_t mp_reach_entries_len = 0;
    bgp_nlri_entry_t *mp_unreach_entries = NULL;
    uint32_t mp_unreach_entries_len = 0;
    mp_reach_info_t mp_reach = {0};
    mp_unreach_info_t mp_unreach = {0};

    /* ----------------------------------------------------------------
     * 1. 读取 Withdrawn Routes Length（WRL，2 字节，大端）
     * ---------------------------------------------------------------- */
    uint16_t wrl = ((uint16_t)body[0] << 8) | body[1];
    if ((uint32_t)(2 + wrl) > body_len)
    {
        goto err;
    }

    /* ----------------------------------------------------------------
     * 2. 解析 IPv4 Withdrawn NLRI（legacy 字段）
     * ---------------------------------------------------------------- */

    if (wrl > 0)
    {
        const bgp_af_parser_t *p = bgp_af_parser_find(BGP_AFI_IPV4, BGP_SAFI_UNICAST);
        if (p && p->parse_unreach)
        {
            p->parse_unreach(body + 2, wrl, &ipv4_wd, &ipv4_wd_len);
        }
    }

    /* ----------------------------------------------------------------
     * 3. 读取 Total Path Attributes Length（PAL，2 字节，大端）
     * ---------------------------------------------------------------- */
    uint32_t pal_off = 2 + wrl;
    if (pal_off + 2 > body_len)
    {
        goto merge; /* UPDATE 中可能只有 Withdrawn 无属性 */
    }

    uint16_t pal = ((uint16_t)body[pal_off] << 8) | body[pal_off + 1];
    uint32_t attr_off = pal_off + 2;

    if (attr_off + pal > body_len)
    {
        goto merge;
    }

    /* ----------------------------------------------------------------
     * 4. 解析路径属性
     * ---------------------------------------------------------------- */
    if (pal > 0)
    {
        bgp_parse_path_attrs(body + attr_off, pal, flags, &r->attr, &r->nexthop, &mp_reach, &mp_unreach);
    }

    /* ----------------------------------------------------------------
     * 5. 解析 IPv4 legacy NLRI（位于报文尾部）
     * ---------------------------------------------------------------- */

    uint32_t nlri_off = attr_off + pal;
    if (nlri_off < body_len)
    {
        const bgp_af_parser_t *p = bgp_af_parser_find(BGP_AFI_IPV4, BGP_SAFI_UNICAST);
        if (p && p->parse_reach)
        {
            p->parse_reach(body + nlri_off, (uint16_t)(body_len - nlri_off), &ipv4_reach, &ipv4_reach_len);
            fill_afi_safi(ipv4_reach, ipv4_reach_len, BGP_AFI_IPV4, BGP_SAFI_UNICAST);
        }
    }

    /* ----------------------------------------------------------------
     * 6. 处理 MP_REACH_NLRI
     * ---------------------------------------------------------------- */
    if (mp_reach.valid)
    {
        r->afi = mp_reach.afi;
        r->safi = mp_reach.safi;

        const bgp_af_parser_t *p = bgp_af_parser_find(mp_reach.afi, mp_reach.safi);
        if (p)
        {
            /* 解析 MP nexthop（覆盖 NEXT_HOP 属性，更精确） */
            if (p->parse_nexthop && mp_reach.nh_len > 0)
            {
                p->parse_nexthop(mp_reach.nh_data, mp_reach.nh_len, &r->nexthop);
            }

            if (p->parse_reach && mp_reach.nlri_len > 0)
            {
                p->parse_reach(mp_reach.nlri_data, mp_reach.nlri_len, &mp_reach_entries, &mp_reach_entries_len);
                fill_afi_safi(mp_reach_entries, mp_reach_entries_len, mp_reach.afi, mp_reach.safi);
            }
        }
        else
        {
            /* 未注册的 AFI/SAFI：保存为 opaque */
            if (mp_reach.nlri_len > 0)
            {
                mp_reach_entries = calloc(1, sizeof(bgp_nlri_entry_t));
                if (mp_reach_entries)
                {
                    mp_reach_entries->afi = mp_reach.afi;
                    mp_reach_entries->safi = mp_reach.safi;
                    mp_reach_entries->type = BGP_NLRI_OPAQUE;
                    uint16_t copylen = mp_reach.nlri_len;
                    if (copylen > sizeof(mp_reach_entries->opaque.data))
                    {
                        copylen = sizeof(mp_reach_entries->opaque.data);
                    }
                    memcpy(mp_reach_entries->opaque.data, mp_reach.nlri_data, copylen);
                    mp_reach_entries->opaque.len = copylen;
                    snprintf(mp_reach_entries->key, BGP_NLRI_KEY_MAX, "opaque:afi=%u:safi=%u", mp_reach.afi,
                             mp_reach.safi);
                    mp_reach_entries_len = 1;
                }
            }
        }
    }
    else if (ipv4_reach_len > 0)
    {
        /* legacy IPv4 NLRI → 主 AFI/SAFI */
        r->afi = BGP_AFI_IPV4;
        r->safi = BGP_SAFI_UNICAST;
    }

    /* ----------------------------------------------------------------
     * 7. 处理 MP_UNREACH_NLRI
     * ---------------------------------------------------------------- */
    if (mp_unreach.valid)
    {
        const bgp_af_parser_t *p = bgp_af_parser_find(mp_unreach.afi, mp_unreach.safi);
        if (p && p->parse_unreach && mp_unreach.nlri_len > 0)
        {
            p->parse_unreach(mp_unreach.nlri_data, mp_unreach.nlri_len, &mp_unreach_entries, &mp_unreach_entries_len);
            fill_afi_safi(mp_unreach_entries, mp_unreach_entries_len, mp_unreach.afi, mp_unreach.safi);
        }
    }

    /* ----------------------------------------------------------------
     * 8. 合并 reach 和 unreach 数组
     * ---------------------------------------------------------------- */
merge:;
    bgp_nlri_entry_t *merged_reach;
    uint32_t merged_reach_len;
    merged_reach = merge_nlri(ipv4_reach, ipv4_reach_len, mp_reach_entries, mp_reach_entries_len, &merged_reach_len);

    bgp_nlri_entry_t *merged_unreach;
    uint32_t merged_unreach_len;
    merged_unreach = merge_nlri(ipv4_wd, ipv4_wd_len, mp_unreach_entries, mp_unreach_entries_len, &merged_unreach_len);

    free(ipv4_reach);
    free(ipv4_wd);
    free(mp_reach_entries);
    free(mp_unreach_entries);

    r->reach = merged_reach;
    r->reach_len = merged_reach_len;
    r->unreach = merged_unreach;
    r->unreach_len = merged_unreach_len;

    *result = r;
    return 0;

err:
    free(r);
    return -1;
}

/* ============================================================================
 * bgp_update_result_free
 * ========================================================================== */

void bgp_update_result_free(bgp_update_result_t *result)
{
    if (!result)
    {
        return;
    }
    free(result->reach);
    free(result->unreach);
    free(result);
}

/* ============================================================================
 * bgp_parse_init：注册所有内置处理器
 * ========================================================================== */

void bgp_parse_init(void)
{
    bgp_parse_ipv4uc_register();
    bgp_parse_ipv6uc_register();
    bgp_parse_labeled_register();
    bgp_parse_vpn_register();
    bgp_parse_evpn_register();
    bgp_parse_flowspec_register();
}
