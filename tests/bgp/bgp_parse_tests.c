#include "bgp_parse.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BUF_MAX 4096

#define TEST_ASSERT(cond)                                                                  \
    do                                                                                     \
    {                                                                                      \
        if (!(cond))                                                                       \
        {                                                                                  \
            fprintf(stderr, "    assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            return -1;                                                                     \
        }                                                                                  \
    } while (0)

typedef struct test_buf
{
    uint8_t  data[TEST_BUF_MAX];
    uint16_t len;
} test_buf_t;

static void buf_require(const test_buf_t *buf, uint16_t need)
{
    if ((uint32_t)buf->len + need > sizeof(buf->data))
    {
        fprintf(stderr, "internal test buffer overflow\n");
        abort();
    }
}

static void buf_reset(test_buf_t *buf)
{
    buf->len = 0;
}

static void buf_u8(test_buf_t *buf, uint8_t v)
{
    buf_require(buf, 1);
    buf->data[buf->len++] = v;
}

static void buf_be16(test_buf_t *buf, uint16_t v)
{
    buf_require(buf, 2);
    buf->data[buf->len++] = (uint8_t)(v >> 8);
    buf->data[buf->len++] = (uint8_t)(v & 0xFF);
}

static void buf_bytes(test_buf_t *buf, const uint8_t *data, uint16_t n)
{
    if (n == 0)
    {
        return;
    }

    buf_require(buf, n);
    memcpy(buf->data + buf->len, data, n);
    buf->len += n;
}

static void buf_attr(test_buf_t *attrs, uint8_t flags, uint8_t type,
                     const uint8_t *val, uint16_t val_len)
{
    uint8_t eff_flags = flags;

    if (val_len > UINT8_MAX)
    {
        eff_flags = (uint8_t)(eff_flags | 0x10);
    }

    buf_u8(attrs, eff_flags);
    buf_u8(attrs, type);

    if ((eff_flags & 0x10) != 0)
    {
        buf_be16(attrs, val_len);
    }
    else
    {
        buf_u8(attrs, (uint8_t)val_len);
    }

    buf_bytes(attrs, val, val_len);
}

static void buf_cap(test_buf_t *caps, uint8_t code, const uint8_t *val, uint8_t val_len)
{
    buf_u8(caps, code);
    buf_u8(caps, val_len);
    buf_bytes(caps, val, val_len);
}

static void build_update_body(test_buf_t *out,
                              const uint8_t *withdrawn, uint16_t withdrawn_len,
                              const test_buf_t *attrs,
                              const uint8_t *nlri, uint16_t nlri_len)
{
    buf_reset(out);
    buf_be16(out, withdrawn_len);
    buf_bytes(out, withdrawn, withdrawn_len);
    buf_be16(out, attrs ? attrs->len : 0);
    if (attrs && attrs->len > 0)
    {
        buf_bytes(out, attrs->data, attrs->len);
    }
    buf_bytes(out, nlri, nlri_len);
}

static void write_be32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)(value & 0xFF);
}

static void encode_label(uint32_t label, uint8_t out[3])
{
    out[0] = (uint8_t)((label >> 12) & 0xFF);
    out[1] = (uint8_t)((label >> 4) & 0xFF);
    out[2] = (uint8_t)(((label & 0x0F) << 4) | 0x01);
}

static bool addr_is_v4(const net_addr_t *addr, const char *ip)
{
    struct in_addr expected;
    if (inet_pton(AF_INET, ip, &expected) != 1)
    {
        return false;
    }
    return addr->family == AF_INET &&
           memcmp(&addr->u.v4, &expected, sizeof(expected)) == 0;
}

static bool addr_is_v6(const net_addr_t *addr, const char *ip)
{
    struct in6_addr expected;
    if (inet_pton(AF_INET6, ip, &expected) != 1)
    {
        return false;
    }
    return addr->family == AF_INET6 &&
           memcmp(&addr->u.v6, &expected, sizeof(expected)) == 0;
}

static int test_af_registry(void)
{
    bgp_parse_init();

    TEST_ASSERT(bgp_af_parser_find(BGP_AFI_IPV4, BGP_SAFI_UNICAST) != NULL);
    TEST_ASSERT(bgp_af_parser_find(BGP_AFI_IPV6, BGP_SAFI_UNICAST) != NULL);
    TEST_ASSERT(bgp_af_parser_find(BGP_AFI_IPV4, BGP_SAFI_LABELED) != NULL);
    TEST_ASSERT(bgp_af_parser_find(BGP_AFI_IPV4, BGP_SAFI_VPN_UNICAST) != NULL);
    TEST_ASSERT(bgp_af_parser_find(BGP_AFI_L2VPN, BGP_SAFI_EVPN) != NULL);
    TEST_ASSERT(bgp_af_parser_find(BGP_AFI_IPV4, BGP_SAFI_FLOWSPEC) != NULL);
    TEST_ASSERT(bgp_af_parser_find(999, 250) == NULL);
    return 0;
}

static int test_pdu_parse_hdr(void)
{
    uint8_t pdu[BGP_HEADER_LEN + 4] = {0};
    memset(pdu, 0xFF, 16);
    pdu[16] = 0x00;
    pdu[17] = (uint8_t)sizeof(pdu);
    pdu[18] = BGP_MSG_TYPE_OPEN;
    pdu[19] = 0x11;
    pdu[20] = 0x22;
    pdu[21] = 0x33;
    pdu[22] = 0x44;

    bgp_pdu_info_t info = {0};
    TEST_ASSERT(bgp_pdu_parse_hdr(pdu, sizeof(pdu), &info) == 0);
    TEST_ASSERT(info.msg_type == BGP_MSG_TYPE_OPEN);
    TEST_ASSERT(info.msg_len == sizeof(pdu));
    TEST_ASSERT(info.body == pdu + BGP_HEADER_LEN);
    TEST_ASSERT(info.body_len == 4);

    pdu[0] = 0x00;
    TEST_ASSERT(bgp_pdu_parse_hdr(pdu, sizeof(pdu), &info) == -1);

    pdu[0]  = 0xFF;
    pdu[16] = 0x00;
    pdu[17] = (uint8_t)(BGP_HEADER_LEN - 1);
    TEST_ASSERT(bgp_pdu_parse_hdr(pdu, sizeof(pdu), &info) == -1);

    pdu[16] = 0x00;
    pdu[17] = (uint8_t)(sizeof(pdu) + 1);
    TEST_ASSERT(bgp_pdu_parse_hdr(pdu, sizeof(pdu), &info) == -1);

    return 0;
}

static int test_open_parse_capabilities(void)
{
    test_buf_t caps = {0};
    test_buf_t body = {0};

    const uint8_t mp_ext[] = {0x00, 0x01, 0x00, 0x01};
    const uint8_t role[]   = {0x02};
    const uint8_t gr[]     = {0x80, 0x78, 0x00, 0x01, 0x01, 0x80};
    const uint8_t as4[]    = {0x00, 0x01, 0x00, 0x00};
    const uint8_t addp[]   = {0x00, 0x02, 0x01, 0x03};
    const uint8_t fqdn[]   = {4, 'e', 'd', 'g', 'e', 11, 'e', 'x', 'a', 'm',
                              'p', 'l', 'e', '.', 'c', 'o', 'm'};

    buf_cap(&caps, BGP_CAP_MP_EXTENSIONS, mp_ext, sizeof(mp_ext));
    buf_cap(&caps, BGP_CAP_ROUTE_REFRESH, NULL, 0);
    buf_cap(&caps, BGP_CAP_EXT_MESSAGE, NULL, 0);
    buf_cap(&caps, BGP_CAP_BGP_ROLE, role, sizeof(role));
    buf_cap(&caps, BGP_CAP_GRACEFUL_RESTART, gr, sizeof(gr));
    buf_cap(&caps, BGP_CAP_AS4, as4, sizeof(as4));
    buf_cap(&caps, BGP_CAP_ADD_PATH, addp, sizeof(addp));
    buf_cap(&caps, BGP_CAP_ENHANCED_RR, NULL, 0);
    buf_cap(&caps, BGP_CAP_FQDN, fqdn, sizeof(fqdn));

    buf_u8(&body, 4);
    buf_be16(&body, 65000);
    buf_be16(&body, 90);
    {
        const uint8_t bgp_id[] = {1, 2, 3, 4};
        buf_bytes(&body, bgp_id, sizeof(bgp_id));
    }
    buf_u8(&body, (uint8_t)(2 + caps.len));
    buf_u8(&body, 2);
    buf_u8(&body, (uint8_t)caps.len);
    buf_bytes(&body, caps.data, caps.len);

    bgp_open_msg_t msg = {0};
    TEST_ASSERT(bgp_open_parse(body.data, body.len, &msg) == 0);

    TEST_ASSERT(msg.version == 4);
    TEST_ASSERT(msg.my_as == 65000);
    TEST_ASSERT(msg.hold_time == 90);
    TEST_ASSERT(strcmp(msg.bgp_id, "1.2.3.4") == 0);

    TEST_ASSERT(msg.mp_count == 1);
    TEST_ASSERT(msg.mp_afs[0] == BGP_AFI_IPV4);
    TEST_ASSERT(msg.mp_safis[0] == BGP_SAFI_UNICAST);

    TEST_ASSERT(msg.cap_route_refresh);
    TEST_ASSERT(msg.cap_ext_message);
    TEST_ASSERT(msg.cap_bgp_role == 2);
    TEST_ASSERT(msg.cap_graceful_restart);
    TEST_ASSERT(msg.graceful_restart.restart_flag);
    TEST_ASSERT(!msg.graceful_restart.notification_flag);
    TEST_ASSERT(msg.graceful_restart.restart_time == 120);
    TEST_ASSERT(msg.graceful_restart.af_count == 1);
    TEST_ASSERT(msg.graceful_restart.afs[0].afi == BGP_AFI_IPV4);
    TEST_ASSERT(msg.graceful_restart.afs[0].safi == BGP_SAFI_UNICAST);
    TEST_ASSERT(msg.graceful_restart.afs[0].forwarding_state);

    TEST_ASSERT(msg.cap_as4 == 65536U);
    TEST_ASSERT(msg.cap_add_path);
    TEST_ASSERT(msg.add_path_count == 1);
    TEST_ASSERT(msg.add_path[0].afi == BGP_AFI_IPV6);
    TEST_ASSERT(msg.add_path[0].safi == BGP_SAFI_UNICAST);
    TEST_ASSERT(msg.add_path[0].flags == (BGP_ADDPATH_RECEIVE | BGP_ADDPATH_SEND));
    TEST_ASSERT(msg.cap_enhanced_rr);
    TEST_ASSERT(strcmp(msg.hostname, "edge") == 0);
    TEST_ASSERT(strcmp(msg.domain, "example.com") == 0);

    {
        uint8_t bad_ver[TEST_BUF_MAX] = {0};
        memcpy(bad_ver, body.data, body.len);
        bad_ver[0] = 3;
        TEST_ASSERT(bgp_open_parse(bad_ver, body.len, &msg) == -1);
    }

    return 0;
}

static int test_notif_parse(void)
{
    uint8_t body[] = {BGP_ERR_CEASE, BGP_CEASE_ADMIN_SHUTDOWN, 0x12, 0x34};
    bgp_notif_msg_t msg = {0};

    TEST_ASSERT(bgp_notif_parse(body, sizeof(body), &msg) == 0);
    TEST_ASSERT(msg.error_code == BGP_ERR_CEASE);
    TEST_ASSERT(msg.error_subcode == BGP_CEASE_ADMIN_SHUTDOWN);
    TEST_ASSERT(msg.data_len == 2);
    TEST_ASSERT(msg.data[0] == 0x12);
    TEST_ASSERT(msg.data[1] == 0x34);
    TEST_ASSERT(strcmp(msg.error_str, "Cease/Admin-Shutdown") == 0);
    TEST_ASSERT(strcmp(bgp_notif_error_str(BGP_ERR_UPDATE, 10),
                       "Update/Invalid-Network-Field") == 0);

    {
        uint8_t trunc[BGP_NOTIF_DATA_MAX + 16] = {0};
        trunc[0] = BGP_ERR_OPEN;
        trunc[1] = BGP_OPEN_ERR_UNSUPPORTED_CAP;
        for (size_t i = 2; i < sizeof(trunc); i++)
        {
            trunc[i] = (uint8_t)i;
        }
        TEST_ASSERT(bgp_notif_parse(trunc, sizeof(trunc), &msg) == 0);
        TEST_ASSERT(msg.data_len == BGP_NOTIF_DATA_MAX);
    }

    return 0;
}

static int test_update_legacy_ipv4(void)
{
    test_buf_t attrs = {0};
    test_buf_t body  = {0};

    const uint8_t withdrawn[] = {24, 203, 0, 113};
    const uint8_t nlri[]      = {16, 10, 10};
    const uint8_t origin[]    = {BGP_ORIGIN_IGP};
    const uint8_t as_path[]   = {
        2, 2,
        0x00, 0x00, 0xFD, 0xE8,
        0x00, 0x00, 0xFD, 0xE9,
    };
    const uint8_t next_hop[]  = {192, 0, 2, 1};
    const uint8_t agg[]       = {0x00, 0x00, 0xFD, 0xF2, 198, 51, 100, 1};
    const uint8_t comm[]      = {0xFF, 0xFF, 0xFF, 0x01};
    const uint8_t originator[] = {198, 51, 100, 9};
    const uint8_t ext_comm[]  = {0x00, 0x02, 0xFD, 0xE8, 0x00, 0x00, 0x00, 0x64};
    const uint8_t large_comm[] = {
        0x00, 0x00, 0xFD, 0xE8,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x02,
    };
    uint8_t med[4] = {0};
    uint8_t local_pref[4] = {0};

    write_be32(med, 100);
    write_be32(local_pref, 200);

    buf_attr(&attrs, 0x40, 1, origin, sizeof(origin));
    buf_attr(&attrs, 0x40, 2, as_path, sizeof(as_path));
    buf_attr(&attrs, 0x40, 3, next_hop, sizeof(next_hop));
    buf_attr(&attrs, 0x80, 4, med, sizeof(med));
    buf_attr(&attrs, 0x40, 5, local_pref, sizeof(local_pref));
    buf_attr(&attrs, 0x40, 6, NULL, 0);
    buf_attr(&attrs, 0xC0, 7, agg, sizeof(agg));
    buf_attr(&attrs, 0xC0, 8, comm, sizeof(comm));
    buf_attr(&attrs, 0x80, 9, originator, sizeof(originator));
    buf_attr(&attrs, 0xC0, 16, ext_comm, sizeof(ext_comm));
    buf_attr(&attrs, 0xC0, 32, large_comm, sizeof(large_comm));

    build_update_body(&body, withdrawn, sizeof(withdrawn), &attrs, nlri, sizeof(nlri));

    bgp_update_result_t *res = NULL;
    TEST_ASSERT(bgp_update_parse(body.data, body.len, 0, &res) == 0);
    TEST_ASSERT(res != NULL);

    TEST_ASSERT(res->afi == BGP_AFI_IPV4);
    TEST_ASSERT(res->safi == BGP_SAFI_UNICAST);
    TEST_ASSERT(res->reach_len == 1);
    TEST_ASSERT(res->unreach_len == 1);

    TEST_ASSERT(res->reach[0].type == BGP_NLRI_PREFIX);
    TEST_ASSERT(res->reach[0].prefix.prefix.prefix_len == 16);
    TEST_ASSERT(addr_is_v4(&res->reach[0].prefix.prefix.addr, "10.10.0.0"));
    TEST_ASSERT(strcmp(res->reach[0].key, "10.10.0.0/16") == 0);

    TEST_ASSERT(res->unreach[0].type == BGP_NLRI_PREFIX);
    TEST_ASSERT(res->unreach[0].prefix.prefix.prefix_len == 24);
    TEST_ASSERT(addr_is_v4(&res->unreach[0].prefix.prefix.addr, "203.0.113.0"));
    TEST_ASSERT(strcmp(res->unreach[0].key, "203.0.113.0/24") == 0);

    TEST_ASSERT(addr_is_v4(&res->nexthop.global, "192.0.2.1"));
    TEST_ASSERT(res->attr.origin == BGP_ORIGIN_IGP);
    TEST_ASSERT(strcmp(res->attr.as_path, "65000 65001") == 0);
    TEST_ASSERT(res->attr.has_med && res->attr.med == 100);
    TEST_ASSERT(res->attr.has_local_pref && res->attr.local_pref == 200);
    TEST_ASSERT(res->attr.atomic_aggregate);
    TEST_ASSERT(strcmp(res->attr.aggregator, "65010:198.51.100.1") == 0);
    TEST_ASSERT(strcmp(res->attr.communities, "no-export") == 0);
    TEST_ASSERT(strcmp(res->attr.ext_communities, "rt:65000:100") == 0);
    TEST_ASSERT(strcmp(res->attr.large_communities, "65000:1:2") == 0);
    TEST_ASSERT(res->attr.has_originator_id);
    TEST_ASSERT(addr_is_v4(&res->attr.originator_id, "198.51.100.9"));

    bgp_update_result_free(res);
    return 0;
}

static int test_update_mp_ipv6_reach_unreach(void)
{
    test_buf_t mp_reach = {0};
    test_buf_t mp_unreach = {0};
    test_buf_t attrs = {0};
    test_buf_t body = {0};

    const uint8_t reach_nlri[]   = {48, 0x20, 0x01, 0x0D, 0xB8, 0x00, 0x01};
    const uint8_t unreach_nlri[] = {48, 0x20, 0x01, 0x0D, 0xB8, 0x00, 0x02};
    uint8_t nh6[16] = {0};

    TEST_ASSERT(inet_pton(AF_INET6, "2001:db8::1", nh6) == 1);

    buf_be16(&mp_reach, BGP_AFI_IPV6);
    buf_u8(&mp_reach, BGP_SAFI_UNICAST);
    buf_u8(&mp_reach, 16);
    buf_bytes(&mp_reach, nh6, sizeof(nh6));
    buf_u8(&mp_reach, 0);
    buf_bytes(&mp_reach, reach_nlri, sizeof(reach_nlri));

    buf_be16(&mp_unreach, BGP_AFI_IPV6);
    buf_u8(&mp_unreach, BGP_SAFI_UNICAST);
    buf_bytes(&mp_unreach, unreach_nlri, sizeof(unreach_nlri));

    buf_attr(&attrs, 0x80, 14, mp_reach.data, mp_reach.len);
    buf_attr(&attrs, 0x80, 15, mp_unreach.data, mp_unreach.len);

    build_update_body(&body, NULL, 0, &attrs, NULL, 0);

    bgp_update_result_t *res = NULL;
    TEST_ASSERT(bgp_update_parse(body.data, body.len, 0, &res) == 0);
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(res->afi == BGP_AFI_IPV6);
    TEST_ASSERT(res->safi == BGP_SAFI_UNICAST);

    TEST_ASSERT(res->reach_len == 1);
    TEST_ASSERT(res->reach[0].type == BGP_NLRI_PREFIX);
    TEST_ASSERT(res->reach[0].prefix.prefix.prefix_len == 48);
    TEST_ASSERT(addr_is_v6(&res->reach[0].prefix.prefix.addr, "2001:db8:1::"));

    TEST_ASSERT(res->unreach_len == 1);
    TEST_ASSERT(res->unreach[0].type == BGP_NLRI_PREFIX);
    TEST_ASSERT(res->unreach[0].prefix.prefix.prefix_len == 48);
    TEST_ASSERT(addr_is_v6(&res->unreach[0].prefix.prefix.addr, "2001:db8:2::"));

    TEST_ASSERT(addr_is_v6(&res->nexthop.global, "2001:db8::1"));
    TEST_ASSERT(!res->nexthop.has_link_local);

    bgp_update_result_free(res);
    return 0;
}

static int test_update_unknown_af_opaque(void)
{
    test_buf_t mp_reach = {0};
    test_buf_t attrs = {0};
    test_buf_t body = {0};

    const uint8_t nh[]   = {1, 1, 1, 1};
    const uint8_t nlri[] = {0xAA, 0xBB};

    buf_be16(&mp_reach, 999);
    buf_u8(&mp_reach, 250);
    buf_u8(&mp_reach, 4);
    buf_bytes(&mp_reach, nh, sizeof(nh));
    buf_u8(&mp_reach, 0);
    buf_bytes(&mp_reach, nlri, sizeof(nlri));

    buf_attr(&attrs, 0x80, 14, mp_reach.data, mp_reach.len);
    build_update_body(&body, NULL, 0, &attrs, NULL, 0);

    bgp_update_result_t *res = NULL;
    TEST_ASSERT(bgp_update_parse(body.data, body.len, 0, &res) == 0);
    TEST_ASSERT(res != NULL);

    TEST_ASSERT(res->afi == 999);
    TEST_ASSERT(res->safi == 250);
    TEST_ASSERT(res->reach_len == 1);
    TEST_ASSERT(res->reach[0].type == BGP_NLRI_OPAQUE);
    TEST_ASSERT(res->reach[0].opaque.len == sizeof(nlri));
    TEST_ASSERT(memcmp(res->reach[0].opaque.data, nlri, sizeof(nlri)) == 0);
    TEST_ASSERT(strcmp(res->reach[0].key, "opaque:afi=999:safi=250") == 0);

    bgp_update_result_free(res);
    return 0;
}

static int test_update_labeled_unicast(void)
{
    test_buf_t mp_reach = {0};
    test_buf_t attrs = {0};
    test_buf_t body = {0};
    uint8_t nlri[7] = {0};
    const uint8_t nh[] = {198, 51, 100, 1};

    nlri[0] = 48;
    encode_label(200, nlri + 1);
    nlri[4] = 192;
    nlri[5] = 0;
    nlri[6] = 2;

    buf_be16(&mp_reach, BGP_AFI_IPV4);
    buf_u8(&mp_reach, BGP_SAFI_LABELED);
    buf_u8(&mp_reach, 4);
    buf_bytes(&mp_reach, nh, sizeof(nh));
    buf_u8(&mp_reach, 0);
    buf_bytes(&mp_reach, nlri, sizeof(nlri));

    buf_attr(&attrs, 0x80, 14, mp_reach.data, mp_reach.len);
    build_update_body(&body, NULL, 0, &attrs, NULL, 0);

    bgp_update_result_t *res = NULL;
    TEST_ASSERT(bgp_update_parse(body.data, body.len, 0, &res) == 0);
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(res->afi == BGP_AFI_IPV4);
    TEST_ASSERT(res->safi == BGP_SAFI_LABELED);
    TEST_ASSERT(res->reach_len == 1);
    TEST_ASSERT(res->reach[0].type == BGP_NLRI_PREFIX);
    TEST_ASSERT(res->reach[0].prefix.has_label);
    TEST_ASSERT(res->reach[0].prefix.label == 200);
    TEST_ASSERT(res->reach[0].prefix.prefix.prefix_len == 24);
    TEST_ASSERT(addr_is_v4(&res->reach[0].prefix.prefix.addr, "192.0.2.0"));
    TEST_ASSERT(strcmp(res->reach[0].key, "192.0.2.0/24 label=200") == 0);
    TEST_ASSERT(addr_is_v4(&res->nexthop.global, "198.51.100.1"));

    bgp_update_result_free(res);
    return 0;
}

static int test_update_vpnv4_unicast(void)
{
    test_buf_t mp_reach = {0};
    test_buf_t attrs = {0};
    test_buf_t body = {0};
    uint8_t nlri[15] = {0};
    const uint8_t rd[] = {0x00, 0x00, 0xFD, 0xE8, 0x00, 0x00, 0x00, 0x01};
    const uint8_t nh[] = {
        0x00, 0x00, 0xFD, 0xE8, 0x00, 0x00, 0x00, 0x01,
        203, 0, 113, 1,
    };

    nlri[0] = 112;
    encode_label(100, nlri + 1);
    memcpy(nlri + 4, rd, sizeof(rd));
    nlri[12] = 10;
    nlri[13] = 0;
    nlri[14] = 1;

    buf_be16(&mp_reach, BGP_AFI_IPV4);
    buf_u8(&mp_reach, BGP_SAFI_VPN_UNICAST);
    buf_u8(&mp_reach, sizeof(nh));
    buf_bytes(&mp_reach, nh, sizeof(nh));
    buf_u8(&mp_reach, 0);
    buf_bytes(&mp_reach, nlri, sizeof(nlri));

    buf_attr(&attrs, 0x80, 14, mp_reach.data, mp_reach.len);
    build_update_body(&body, NULL, 0, &attrs, NULL, 0);

    bgp_update_result_t *res = NULL;
    TEST_ASSERT(bgp_update_parse(body.data, body.len, 0, &res) == 0);
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(res->afi == BGP_AFI_IPV4);
    TEST_ASSERT(res->safi == BGP_SAFI_VPN_UNICAST);
    TEST_ASSERT(res->reach_len == 1);
    TEST_ASSERT(res->reach[0].type == BGP_NLRI_PREFIX);
    TEST_ASSERT(res->reach[0].prefix.has_label);
    TEST_ASSERT(res->reach[0].prefix.label == 100);
    TEST_ASSERT(res->reach[0].prefix.has_rd);
    TEST_ASSERT(memcmp(res->reach[0].prefix.rd.bytes, rd, sizeof(rd)) == 0);
    TEST_ASSERT(res->reach[0].prefix.prefix.prefix_len == 24);
    TEST_ASSERT(addr_is_v4(&res->reach[0].prefix.prefix.addr, "10.0.1.0"));
    TEST_ASSERT(addr_is_v4(&res->nexthop.global, "203.0.113.1"));
    TEST_ASSERT(strstr(res->reach[0].key, "vpn:65000:1:10.0.1.0/24") != NULL);

    bgp_update_result_free(res);
    return 0;
}

static int test_update_evpn_type2(void)
{
    test_buf_t mp_reach = {0};
    test_buf_t attrs = {0};
    test_buf_t body = {0};
    const uint8_t rd[] = {0x00, 0x00, 0xFD, 0xE8, 0x00, 0x00, 0x00, 0x64};
    const uint8_t mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const uint8_t nh[] = {192, 0, 2, 9};
    uint8_t evpn_val[33] = {0};
    uint8_t nlri[35] = {0};
    uint8_t label[3] = {0};
    uint16_t pos = 0;

    encode_label(300, label);

    memcpy(evpn_val + pos, rd, sizeof(rd));
    pos += sizeof(rd);
    memset(evpn_val + pos, 0, 10);
    pos += 10;

    evpn_val[pos++] = 0;
    evpn_val[pos++] = 0;
    evpn_val[pos++] = 0;
    evpn_val[pos++] = 42;

    evpn_val[pos++] = 48;
    memcpy(evpn_val + pos, mac, sizeof(mac));
    pos += sizeof(mac);
    evpn_val[pos++] = 0;
    memcpy(evpn_val + pos, label, sizeof(label));
    pos += sizeof(label);

    TEST_ASSERT(pos == sizeof(evpn_val));

    nlri[0] = 2;
    nlri[1] = sizeof(evpn_val);
    memcpy(nlri + 2, evpn_val, sizeof(evpn_val));

    buf_be16(&mp_reach, BGP_AFI_L2VPN);
    buf_u8(&mp_reach, BGP_SAFI_EVPN);
    buf_u8(&mp_reach, 4);
    buf_bytes(&mp_reach, nh, sizeof(nh));
    buf_u8(&mp_reach, 0);
    buf_bytes(&mp_reach, nlri, sizeof(nlri));

    buf_attr(&attrs, 0x80, 14, mp_reach.data, mp_reach.len);
    build_update_body(&body, NULL, 0, &attrs, NULL, 0);

    bgp_update_result_t *res = NULL;
    TEST_ASSERT(bgp_update_parse(body.data, body.len, 0, &res) == 0);
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(res->afi == BGP_AFI_L2VPN);
    TEST_ASSERT(res->safi == BGP_SAFI_EVPN);
    TEST_ASSERT(res->reach_len == 1);
    TEST_ASSERT(res->reach[0].type == BGP_NLRI_EVPN);
    TEST_ASSERT(res->reach[0].evpn.route_type == 2);
    TEST_ASSERT(res->reach[0].evpn.has_mac);
    TEST_ASSERT(!res->reach[0].evpn.has_ip);
    TEST_ASSERT(memcmp(res->reach[0].evpn.mac, mac, sizeof(mac)) == 0);
    TEST_ASSERT(res->reach[0].evpn.label1 == 300);
    TEST_ASSERT(strstr(res->reach[0].key, "evpn:2:") != NULL);
    TEST_ASSERT(strstr(res->reach[0].key, "aa:bb:cc:dd:ee:ff") != NULL);

    bgp_update_result_free(res);
    return 0;
}

static int test_update_flowspec(void)
{
    test_buf_t mp_reach = {0};
    test_buf_t attrs = {0};
    test_buf_t body = {0};
    const uint8_t nh[] = {198, 51, 100, 7};
    const uint8_t comps[] = {
        0x01, 0x18, 0xC0, 0x00, 0x02,
        0x03, 0x82, 0x06,
    };
    uint8_t nlri[1 + sizeof(comps)] = {0};

    nlri[0] = sizeof(comps);
    memcpy(nlri + 1, comps, sizeof(comps));

    buf_be16(&mp_reach, BGP_AFI_IPV4);
    buf_u8(&mp_reach, BGP_SAFI_FLOWSPEC);
    buf_u8(&mp_reach, 4);
    buf_bytes(&mp_reach, nh, sizeof(nh));
    buf_u8(&mp_reach, 0);
    buf_bytes(&mp_reach, nlri, sizeof(nlri));

    buf_attr(&attrs, 0x80, 14, mp_reach.data, mp_reach.len);
    build_update_body(&body, NULL, 0, &attrs, NULL, 0);

    bgp_update_result_t *res = NULL;
    TEST_ASSERT(bgp_update_parse(body.data, body.len, 0, &res) == 0);
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(res->afi == BGP_AFI_IPV4);
    TEST_ASSERT(res->safi == BGP_SAFI_FLOWSPEC);
    TEST_ASSERT(res->reach_len == 1);
    TEST_ASSERT(res->reach[0].type == BGP_NLRI_FLOWSPEC);
    TEST_ASSERT(res->reach[0].flowspec.count == 2);
    TEST_ASSERT(strcmp(res->reach[0].flowspec.components[0].str, "dst=192.0.2.0/24") == 0);
    TEST_ASSERT(strcmp(res->reach[0].flowspec.components[1].str, "proto=tcp") == 0);
    TEST_ASSERT(strcmp(res->reach[0].key, "dst=192.0.2.0/24,proto=tcp") == 0);
    TEST_ASSERT(addr_is_v4(&res->nexthop.global, "198.51.100.7"));

    bgp_update_result_free(res);
    return 0;
}

static int test_update_parse_errors(void)
{
    bgp_update_result_t *res = (bgp_update_result_t *)(uintptr_t)0x1;
    uint8_t short_body[3] = {0};
    uint8_t bad_wrl[4] = {0x00, 0x05, 0x00, 0x00};

    TEST_ASSERT(bgp_update_parse(NULL, 0, 0, &res) == -1);
    TEST_ASSERT(res == NULL);

    res = (bgp_update_result_t *)(uintptr_t)0x1;
    TEST_ASSERT(bgp_update_parse(short_body, sizeof(short_body), 0, &res) == -1);
    TEST_ASSERT(res == NULL);

    TEST_ASSERT(bgp_update_parse(bad_wrl, sizeof(bad_wrl), 0, &res) == -1);
    TEST_ASSERT(res == NULL);

    bgp_update_result_free(NULL);
    return 0;
}

typedef int (*test_fn_t)(void);

typedef struct test_case
{
    const char *name;
    test_fn_t   fn;
} test_case_t;

int main(void)
{
    const test_case_t tests[] = {
        {"af_registry", test_af_registry},
        {"pdu_parse_hdr", test_pdu_parse_hdr},
        {"open_parse_capabilities", test_open_parse_capabilities},
        {"notif_parse", test_notif_parse},
        {"update_legacy_ipv4", test_update_legacy_ipv4},
        {"update_mp_ipv6_reach_unreach", test_update_mp_ipv6_reach_unreach},
        {"update_unknown_af_opaque", test_update_unknown_af_opaque},
        {"update_labeled_unicast", test_update_labeled_unicast},
        {"update_vpnv4_unicast", test_update_vpnv4_unicast},
        {"update_evpn_type2", test_update_evpn_type2},
        {"update_flowspec", test_update_flowspec},
        {"update_parse_errors", test_update_parse_errors},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t fail  = 0;

    bgp_parse_init();

    for (size_t i = 0; i < total; i++)
    {
        int rc = tests[i].fn();
        if (rc == 0)
        {
            printf("[PASS] %s\n", tests[i].name);
        }
        else
        {
            printf("[FAIL] %s\n", tests[i].name);
            fail++;
        }
    }

    printf("\nResult: %zu total, %zu passed, %zu failed\n", total, total - fail, fail);
    return (fail == 0) ? 0 : 1;
}
