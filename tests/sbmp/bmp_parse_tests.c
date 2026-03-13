#include "sbmp_bmp.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(cond)                                                                  \
    do                                                                                     \
    {                                                                                      \
        if (!(cond))                                                                       \
        {                                                                                  \
            fprintf(stderr, "    assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            return -1;                                                                     \
        }                                                                                  \
    } while (0)

static void set_be32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v >> 24);
    dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >> 8);
    dst[3] = (uint8_t)(v & 0xFF);
}

static int test_common_header_ok(void)
{
    uint8_t msg[SBMP_BMP_COMMON_HDR_LEN] = {0};
    sbmp_bmp_common_view_t view;

    msg[0] = 3;
    set_be32(msg + 1, SBMP_BMP_COMMON_HDR_LEN);
    msg[5] = SBMP_BMP_MSG_INITIATION;

    TEST_ASSERT(sbmp_bmp_parse_common_header(msg, sizeof(msg), &view) == 0);
    TEST_ASSERT(view.version == 3);
    TEST_ASSERT(view.msg_len == SBMP_BMP_COMMON_HDR_LEN);
    TEST_ASSERT(view.msg_type == SBMP_BMP_MSG_INITIATION);
    return 0;
}

static int test_common_header_invalid(void)
{
    uint8_t msg[SBMP_BMP_COMMON_HDR_LEN] = {0};
    sbmp_bmp_common_view_t view;

    msg[0] = 2;
    set_be32(msg + 1, SBMP_BMP_COMMON_HDR_LEN);
    msg[5] = SBMP_BMP_MSG_INITIATION;
    TEST_ASSERT(sbmp_bmp_parse_common_header(msg, sizeof(msg), &view) != 0);

    msg[0] = 3;
    set_be32(msg + 1, SBMP_BMP_COMMON_HDR_LEN - 1);
    TEST_ASSERT(sbmp_bmp_parse_common_header(msg, sizeof(msg), &view) != 0);

    msg[0] = 3;
    set_be32(msg + 1, SBMP_BMP_COMMON_HDR_LEN + 10);
    TEST_ASSERT(sbmp_bmp_parse_common_header(msg, sizeof(msg), &view) != 0);

    TEST_ASSERT(sbmp_bmp_parse_common_header(msg, 0, &view) != 0);
    return 0;
}

static int test_peer_header_ipv4_pre(void)
{
    uint8_t peer[SBMP_BMP_PEER_HDR_LEN] = {0};
    sbmp_bmp_peer_view_t view;
    struct in_addr v4;

    inet_pton(AF_INET, "203.0.113.7", &v4);
    memcpy(peer + 22, &v4, sizeof(v4));
    set_be32(peer + 26, 65001);
    inet_pton(AF_INET, "1.2.3.4", peer + 30);

    TEST_ASSERT(sbmp_bmp_parse_peer_header(peer, sizeof(peer), &view) == 0);
    TEST_ASSERT(strcmp(view.peer_ip, "203.0.113.7") == 0);
    TEST_ASSERT(view.peer_as == 65001);
    TEST_ASSERT(strcmp(view.bgp_id, "1.2.3.4") == 0);
    TEST_ASSERT(view.is_ipv6 == 0);
    TEST_ASSERT(view.is_post_policy == 0);
    return 0;
}

static int test_peer_header_ipv6_post(void)
{
    uint8_t peer[SBMP_BMP_PEER_HDR_LEN] = {0};
    sbmp_bmp_peer_view_t view;
    struct in6_addr v6;

    peer[1] = 0xC0; /* V=1, L=1 */
    inet_pton(AF_INET6, "2001:db8::10", &v6);
    memcpy(peer + 10, &v6, sizeof(v6));
    set_be32(peer + 26, 4200000001u);
    inet_pton(AF_INET, "9.8.7.6", peer + 30);

    TEST_ASSERT(sbmp_bmp_parse_peer_header(peer, sizeof(peer), &view) == 0);
    TEST_ASSERT(strcmp(view.peer_ip, "2001:db8::10") == 0);
    TEST_ASSERT(view.peer_as == 4200000001u);
    TEST_ASSERT(strcmp(view.bgp_id, "9.8.7.6") == 0);
    TEST_ASSERT(view.is_ipv6 == 1);
    TEST_ASSERT(view.is_post_policy == 1);
    return 0;
}

static int test_peer_header_loc_rib(void)
{
    uint8_t peer[SBMP_BMP_PEER_HDR_LEN] = {0};
    sbmp_bmp_peer_view_t view;

    peer[0] = SBMP_BMP_PEER_TYPE_LOC_RIB;
    peer[1] = 0x80; /* F=1 */
    peer[9] = 0x01; /* RD = 0x0000000000000001 */
    set_be32(peer + 26, 64512);
    inet_pton(AF_INET, "10.20.30.40", peer + 30);

    TEST_ASSERT(sbmp_bmp_parse_peer_header(peer, sizeof(peer), &view) == 0);
    TEST_ASSERT(view.peer_type == SBMP_BMP_PEER_TYPE_LOC_RIB);
    TEST_ASSERT(view.is_loc_rib == 1);
    TEST_ASSERT(view.is_loc_rib_filtered == 1);
    TEST_ASSERT(view.is_post_policy == 0);
    TEST_ASSERT(view.peer_as == 64512);
    TEST_ASSERT(strcmp(view.bgp_id, "10.20.30.40") == 0);
    TEST_ASSERT(strcmp(view.peer_ip, "loc-rib:10.20.30.40:0000000000000001") == 0);
    return 0;
}

static int test_extract_route_monitoring(void)
{
    uint8_t msg[SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN + 19] = {0};
    sbmp_bmp_peer_view_t peer;
    const uint8_t *bgp_pdu = NULL;
    uint32_t bgp_pdu_len = 0;
    struct in_addr v4;

    msg[0] = 3;
    set_be32(msg + 1, sizeof(msg));
    msg[5] = SBMP_BMP_MSG_ROUTE_MONITORING;

    inet_pton(AF_INET, "198.51.100.1", &v4);
    memcpy(msg + SBMP_BMP_COMMON_HDR_LEN + 22, &v4, sizeof(v4));
    set_be32(msg + SBMP_BMP_COMMON_HDR_LEN + 26, 65100);
    inet_pton(AF_INET, "11.22.33.44", msg + SBMP_BMP_COMMON_HDR_LEN + 30);

    memset(msg + SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN, 0xFF, 16);
    msg[SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN + 16] = 0x00;
    msg[SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN + 17] = 19;
    msg[SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN + 18] = 4; /* KEEPALIVE */

    TEST_ASSERT(sbmp_bmp_extract_route_monitoring(msg, sizeof(msg), &peer, &bgp_pdu, &bgp_pdu_len) == 0);
    TEST_ASSERT(strcmp(peer.peer_ip, "198.51.100.1") == 0);
    TEST_ASSERT(peer.peer_as == 65100);
    TEST_ASSERT(strcmp(peer.bgp_id, "11.22.33.44") == 0);
    TEST_ASSERT(peer.is_post_policy == 0);
    TEST_ASSERT(bgp_pdu != NULL);
    TEST_ASSERT(bgp_pdu_len == 19);
    TEST_ASSERT(bgp_pdu[18] == 4);

    msg[5] = SBMP_BMP_MSG_STATS_REPORT;
    TEST_ASSERT(sbmp_bmp_extract_route_monitoring(msg, sizeof(msg), &peer, &bgp_pdu, &bgp_pdu_len) != 0);
    return 0;
}

static int test_extract_route_monitoring_loc_rib(void)
{
    uint8_t msg[SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN + 19] = {0};
    sbmp_bmp_peer_view_t peer;
    const uint8_t *bgp_pdu = NULL;
    uint32_t bgp_pdu_len = 0;

    msg[0] = 3;
    set_be32(msg + 1, sizeof(msg));
    msg[5] = SBMP_BMP_MSG_ROUTE_MONITORING;

    msg[SBMP_BMP_COMMON_HDR_LEN + 0] = SBMP_BMP_PEER_TYPE_LOC_RIB;
    msg[SBMP_BMP_COMMON_HDR_LEN + 1] = 0x80; /* F=1 */
    msg[SBMP_BMP_COMMON_HDR_LEN + 9] = 0x2A;
    set_be32(msg + SBMP_BMP_COMMON_HDR_LEN + 26, 65000);
    inet_pton(AF_INET, "172.16.0.1", msg + SBMP_BMP_COMMON_HDR_LEN + 30);

    memset(msg + SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN, 0xFF, 16);
    msg[SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN + 16] = 0x00;
    msg[SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN + 17] = 19;
    msg[SBMP_BMP_COMMON_HDR_LEN + SBMP_BMP_PEER_HDR_LEN + 18] = 4;

    TEST_ASSERT(sbmp_bmp_extract_route_monitoring(msg, sizeof(msg), &peer, &bgp_pdu, &bgp_pdu_len) == 0);
    TEST_ASSERT(peer.peer_type == SBMP_BMP_PEER_TYPE_LOC_RIB);
    TEST_ASSERT(peer.is_loc_rib == 1);
    TEST_ASSERT(peer.is_loc_rib_filtered == 1);
    TEST_ASSERT(strcmp(peer.peer_ip, "loc-rib:172.16.0.1:000000000000002a") == 0);
    TEST_ASSERT(bgp_pdu_len == 19);
    return 0;
}

typedef int (*test_fn_t)(void);

typedef struct test_case
{
    const char *name;
    test_fn_t fn;
} test_case_t;

int main(void)
{
    const test_case_t tests[] = {
        {"common_header_ok", test_common_header_ok},
        {"common_header_invalid", test_common_header_invalid},
        {"peer_header_ipv4_pre", test_peer_header_ipv4_pre},
        {"peer_header_ipv6_post", test_peer_header_ipv6_post},
        {"peer_header_loc_rib", test_peer_header_loc_rib},
        {"extract_route_monitoring", test_extract_route_monitoring},
        {"extract_route_monitoring_loc_rib", test_extract_route_monitoring_loc_rib},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t fail = 0;

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
