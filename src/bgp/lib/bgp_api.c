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
            return 0;

        case BGP_NLRI_QP:
            r = (int)a->qp.dqpn_len - (int)b->qp.dqpn_len;
            if (r)
            {
                return r;
            }
            if (a->qp.dqpn < b->qp.dqpn)
            {
                return -1;
            }
            if (a->qp.dqpn > b->qp.dqpn)
            {
                return 1;
            }
            r = net_addr_cmp(&a->qp.prefix.addr, &b->qp.prefix.addr);
            if (r)
            {
                return r;
            }
            return (int)a->qp.prefix.prefix_len - (int)b->qp.prefix.prefix_len;

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

static void nlri_hash_bytes(uint64_t *hash, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++)
    {
        *hash ^= bytes[i];
        *hash *= 1099511628211ULL;
    }
}

uint32_t bgp_nlri_hash(const bgp_nlri_entry_t *entry)
{
    if (!entry)
    {
        return 0u;
    }
    uint64_t h = 1469598103934665603ULL;
    nlri_hash_bytes(&h, &entry->afi, sizeof(entry->afi));
    nlri_hash_bytes(&h, &entry->safi, sizeof(entry->safi));
    nlri_hash_bytes(&h, &entry->type, sizeof(entry->type));
    switch (entry->type)
    {
        case BGP_NLRI_PREFIX:
        {
            guint addr_hash = net_addr_hash(&entry->prefix.prefix.addr);
            nlri_hash_bytes(&h, &addr_hash, sizeof(addr_hash));
            nlri_hash_bytes(&h, &entry->prefix.prefix.prefix_len, sizeof(entry->prefix.prefix.prefix_len));
            nlri_hash_bytes(&h, &entry->prefix.has_rd, sizeof(entry->prefix.has_rd));
            if (entry->prefix.has_rd)
            {
                nlri_hash_bytes(&h, entry->prefix.rd.bytes, sizeof(entry->prefix.rd.bytes));
            }
            break;
        }
        case BGP_NLRI_QP:
        {
            guint addr_hash = net_addr_hash(&entry->qp.prefix.addr);
            nlri_hash_bytes(&h, &entry->qp.dqpn_len, sizeof(entry->qp.dqpn_len));
            nlri_hash_bytes(&h, &entry->qp.dqpn, sizeof(entry->qp.dqpn));
            nlri_hash_bytes(&h, &addr_hash, sizeof(addr_hash));
            nlri_hash_bytes(&h, &entry->qp.prefix.prefix_len, sizeof(entry->qp.prefix.prefix_len));
            break;
        }
        case BGP_NLRI_EVPN:
            nlri_hash_bytes(&h, &entry->evpn.raw_len, sizeof(entry->evpn.raw_len));
            nlri_hash_bytes(&h, entry->evpn.raw, entry->evpn.raw_len);
            break;
        case BGP_NLRI_FLOWSPEC:
            nlri_hash_bytes(&h, &entry->flowspec.has_rd, sizeof(entry->flowspec.has_rd));
            if (entry->flowspec.has_rd)
            {
                nlri_hash_bytes(&h, entry->flowspec.rd.bytes, sizeof(entry->flowspec.rd.bytes));
            }
            nlri_hash_bytes(&h, &entry->flowspec.count, sizeof(entry->flowspec.count));
            for (uint8_t i = 0; i < entry->flowspec.count; i++)
            {
                const bgp_fs_component_t *component = &entry->flowspec.components[i];
                nlri_hash_bytes(&h, &component->type, sizeof(component->type));
                nlri_hash_bytes(&h, &component->data_len, sizeof(component->data_len));
                nlri_hash_bytes(&h, component->data, component->data_len);
            }
            break;
        case BGP_NLRI_OPAQUE:
        default:
            nlri_hash_bytes(&h, &entry->opaque.len, sizeof(entry->opaque.len));
            nlri_hash_bytes(&h, entry->opaque.data, entry->opaque.len);
            break;
    }
    return (uint32_t)h;
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
