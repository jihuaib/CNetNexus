/**
 * @file   bgp_api.c
 * @brief  BGP 模块对外 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <stdio.h>
#include <string.h>

#include "bgp.h"

int bgp_nlri_cmp(const bgp_nlri_entry_t *a, const bgp_nlri_entry_t *b)
{
    if (!a && !b)
    {
        return 0;
    }
    if (!a)
    {
        return -1;
    }
    if (!b)
    {
        return 1;
    }

    int r = (int)a->afi - (int)b->afi;
    if (r)
    {
        return r;
    }
    r = (int)a->safi - (int)b->safi;
    if (r)
    {
        return r;
    }
    if (a->type != b->type)
    {
        return (int)a->type - (int)b->type;
    }

    switch (a->type)
    {
        case BGP_NLRI_PREFIX:
            r = net_addr_cmp(&a->prefix.prefix.addr, &b->prefix.prefix.addr);
            if (r)
            {
                return r;
            }
            r = (int)a->prefix.prefix.prefix_len - (int)b->prefix.prefix.prefix_len;
            if (r)
            {
                return r;
            }
            r = (int)a->prefix.has_rd - (int)b->prefix.has_rd;
            if (r)
            {
                return r;
            }
            if (a->prefix.has_rd)
            {
                r = memcmp(a->prefix.rd.bytes, b->prefix.rd.bytes, sizeof(a->prefix.rd.bytes));
                if (r)
                {
                    return r;
                }
            }
            r = (int)a->prefix.has_label - (int)b->prefix.has_label;
            if (r)
            {
                return r;
            }
            if (a->prefix.has_label)
            {
                if (a->prefix.label < b->prefix.label)
                {
                    return -1;
                }
                if (a->prefix.label > b->prefix.label)
                {
                    return 1;
                }
            }
            return 0;

        case BGP_NLRI_EVPN:
            r = (int)a->evpn.raw_len - (int)b->evpn.raw_len;
            if (r)
            {
                return r;
            }
            return memcmp(a->evpn.raw, b->evpn.raw, a->evpn.raw_len);

        case BGP_NLRI_FLOWSPEC:
            r = (int)a->flowspec.has_rd - (int)b->flowspec.has_rd;
            if (r)
            {
                return r;
            }
            if (a->flowspec.has_rd)
            {
                r = memcmp(a->flowspec.rd.bytes, b->flowspec.rd.bytes, sizeof(a->flowspec.rd.bytes));
                if (r)
                {
                    return r;
                }
            }
            r = (int)a->flowspec.count - (int)b->flowspec.count;
            if (r)
            {
                return r;
            }
            for (uint8_t i = 0; i < a->flowspec.count; i++)
            {
                const bgp_fs_component_t *ca = &a->flowspec.components[i];
                const bgp_fs_component_t *cb = &b->flowspec.components[i];
                r = (int)ca->type - (int)cb->type;
                if (r)
                {
                    return r;
                }
                r = (int)ca->data_len - (int)cb->data_len;
                if (r)
                {
                    return r;
                }
                r = memcmp(ca->data, cb->data, ca->data_len);
                if (r)
                {
                    return r;
                }
            }
            return 0;

        case BGP_NLRI_OPAQUE:
        default:
            r = (int)a->opaque.len - (int)b->opaque.len;
            if (r)
            {
                return r;
            }
            return memcmp(a->opaque.data, b->opaque.data, a->opaque.len);
    }
}

bool bgp_nlri_equal(const bgp_nlri_entry_t *a, const bgp_nlri_entry_t *b)
{
    return bgp_nlri_cmp(a, b) == 0;
}

void bgp_nlri_to_str(const bgp_nlri_entry_t *entry, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    buf[0] = '\0';

    if (!entry)
    {
        snprintf(buf, sz, "<null>");
        return;
    }

    const bgp_af_parser_t *parser = bgp_af_parser_find(entry->afi, entry->safi);
    if (parser && parser->entry_to_str)
    {
        parser->entry_to_str(entry, buf, sz);
    }
}
