#include "bgp_rib.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net_addr.h"

#define TEST_ASSERT(cond)                                                                  \
    do                                                                                     \
    {                                                                                      \
        if (!(cond))                                                                       \
        {                                                                                  \
            fprintf(stderr, "    assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            return -1;                                                                     \
        }                                                                                  \
    } while (0)

/* 解析 "addr/plen" 字符串，同时填写二进制前缀字段（供 nlri_compare 使用） */
static void make_prefix_entry(bgp_nlri_entry_t *e, uint16_t afi, uint8_t safi, const char *prefix)
{
    memset(e, 0, sizeof(*e));
    e->afi  = afi;
    e->safi = safi;
    e->type = BGP_NLRI_PREFIX;

    /* 解析 "addr/plen" → 二进制 */
    char addr_buf[64];
    snprintf(addr_buf, sizeof(addr_buf), "%s", prefix);
    char *slash = strchr(addr_buf, '/');
    if (slash)
    {
        *slash = '\0';
        e->prefix.prefix.prefix_len = (uint8_t)atoi(slash + 1);
    }
    e->prefix.prefix.addr.family = (afi == BGP_AFI_IPV6) ? AF_INET6 : AF_INET;
    inet_pton(e->prefix.prefix.addr.family, addr_buf, &e->prefix.prefix.addr.u);
}

static void make_v4_nexthop(bgp_nexthop_t *nh, const char *ip)
{
    memset(nh, 0, sizeof(*nh));
    nh->global.family = AF_INET;
    inet_pton(AF_INET, ip, &nh->global.u.v4);
}

static net_addr_t make_v4_source(const char *ip)
{
    net_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = AF_INET;
    inet_pton(AF_INET, ip, &src.u.v4);
    return src;
}

static int test_reach_update_unreach(void)
{
    bgp_rib_t *rib = bgp_rib_create();
    TEST_ASSERT(rib != NULL);

    bgp_nlri_entry_t e1;
    bgp_attr_t       attr1 = {0};
    bgp_attr_t       attr2 = {0};
    bgp_nexthop_t    nh = {0};

    make_prefix_entry(&e1, BGP_AFI_IPV4, BGP_SAFI_UNICAST, "10.0.0.0/24");
    make_v4_nexthop(&nh, "192.0.2.1");

    attr1.has_local_pref = true;
    attr1.local_pref     = 100;
    attr2.has_local_pref = true;
    attr2.local_pref     = 200;

    net_addr_t src1 = make_v4_source("198.51.100.1");
    net_addr_t src2 = make_v4_source("198.51.100.2");

    TEST_ASSERT(bgp_rib_reach_one(rib, &e1, &src1, &attr1, &nh) == 1);
    TEST_ASSERT(bgp_rib_head_count(rib) == 1);
    TEST_ASSERT(bgp_rib_route_count(rib) == 1);

    TEST_ASSERT(bgp_rib_reach_one(rib, &e1, &src1, &attr2, &nh) == 0);
    TEST_ASSERT(bgp_rib_head_count(rib) == 1);
    TEST_ASSERT(bgp_rib_route_count(rib) == 1);

    const bgp_rthead_t *head = bgp_rib_lookup_head(rib, &e1);
    TEST_ASSERT(head != NULL);
    const bgp_route_node_t *r1 = bgp_rthead_lookup_route(head, &src1);
    TEST_ASSERT(r1 != NULL);
    TEST_ASSERT(r1->attr.has_local_pref);
    TEST_ASSERT(r1->attr.local_pref == 200);

    TEST_ASSERT(bgp_rib_reach_one(rib, &e1, &src2, &attr1, &nh) == 1);
    TEST_ASSERT(bgp_rib_head_count(rib) == 1);
    TEST_ASSERT(bgp_rib_route_count(rib) == 2);

    TEST_ASSERT(bgp_rib_unreach_one(rib, &e1, &src1) == 1);
    TEST_ASSERT(bgp_rib_head_count(rib) == 1);
    TEST_ASSERT(bgp_rib_route_count(rib) == 1);

    TEST_ASSERT(bgp_rib_unreach_one(rib, &e1, &src2) == 1);
    TEST_ASSERT(bgp_rib_head_count(rib) == 0);
    TEST_ASSERT(bgp_rib_route_count(rib) == 0);

    TEST_ASSERT(bgp_rib_unreach_one(rib, &e1, &src2) == 0);

    bgp_rib_destroy(rib);
    return 0;
}

static int test_remove_source_across_heads(void)
{
    bgp_rib_t *rib = bgp_rib_create();
    TEST_ASSERT(rib != NULL);

    bgp_nlri_entry_t e1, e2;
    bgp_attr_t       attr = {0};
    bgp_nexthop_t    nh = {0};
    uint32_t         removed_routes = 0;
    uint32_t         removed_heads  = 0;

    make_prefix_entry(&e1, BGP_AFI_IPV4, BGP_SAFI_UNICAST, "10.1.0.0/24");
    make_prefix_entry(&e2, BGP_AFI_IPV4, BGP_SAFI_UNICAST, "10.2.0.0/24");
    make_v4_nexthop(&nh, "192.0.2.9");

    net_addr_t src1 = make_v4_source("203.0.113.1");
    net_addr_t src2 = make_v4_source("203.0.113.2");

    TEST_ASSERT(bgp_rib_reach_one(rib, &e1, &src1, &attr, &nh) == 1);
    TEST_ASSERT(bgp_rib_reach_one(rib, &e2, &src1, &attr, &nh) == 1);
    TEST_ASSERT(bgp_rib_reach_one(rib, &e2, &src2, &attr, &nh) == 1);
    TEST_ASSERT(bgp_rib_head_count(rib) == 2);
    TEST_ASSERT(bgp_rib_route_count(rib) == 3);

    bgp_rib_remove_source(rib, &src1, &removed_routes, &removed_heads);
    TEST_ASSERT(removed_routes == 2);
    TEST_ASSERT(removed_heads == 1);
    TEST_ASSERT(bgp_rib_head_count(rib) == 1);
    TEST_ASSERT(bgp_rib_route_count(rib) == 1);

    bgp_rib_destroy(rib);
    return 0;
}

static int test_apply_update_stats(void)
{
    bgp_rib_t *rib = bgp_rib_create();
    TEST_ASSERT(rib != NULL);

    bgp_nlri_entry_t  reach1;
    bgp_nlri_entry_t  reach2;
    bgp_nlri_entry_t  unreach1;
    bgp_nlri_entry_t  reach_arr[2];
    bgp_nlri_entry_t  unreach_arr[1];
    bgp_update_result_t upd = {0};
    bgp_rib_update_stats_t stats = {0};

    make_prefix_entry(&reach1, BGP_AFI_IPV4, BGP_SAFI_UNICAST, "10.3.0.0/24");
    make_prefix_entry(&reach2, BGP_AFI_IPV4, BGP_SAFI_UNICAST, "10.4.0.0/24");
    make_prefix_entry(&unreach1, BGP_AFI_IPV4, BGP_SAFI_UNICAST, "10.4.0.0/24");

    reach_arr[0] = reach1;
    reach_arr[1] = reach2;
    upd.reach = reach_arr;
    upd.reach_len = 2;
    upd.unreach = NULL;
    upd.unreach_len = 0;
    make_v4_nexthop(&upd.nexthop, "192.0.2.5");

    net_addr_t src = make_v4_source("203.0.113.10");

    bgp_rib_apply_update(rib, &src, &upd, &stats);
    TEST_ASSERT(stats.reach_new == 2);
    TEST_ASSERT(stats.reach_update == 0);
    TEST_ASSERT(stats.unreach_removed == 0);
    TEST_ASSERT(stats.unreach_miss == 0);
    TEST_ASSERT(bgp_rib_head_count(rib) == 2);
    TEST_ASSERT(bgp_rib_route_count(rib) == 2);

    upd.reach = &reach1;
    upd.reach_len = 1;
    unreach_arr[0] = unreach1;
    upd.unreach = unreach_arr;
    upd.unreach_len = 1;

    bgp_rib_apply_update(rib, &src, &upd, &stats);
    TEST_ASSERT(stats.reach_new == 0);
    TEST_ASSERT(stats.reach_update == 1);
    TEST_ASSERT(stats.unreach_removed == 1);
    TEST_ASSERT(stats.unreach_miss == 0);
    TEST_ASSERT(bgp_rib_head_count(rib) == 1);
    TEST_ASSERT(bgp_rib_route_count(rib) == 1);

    bgp_rib_destroy(rib);
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
        {"reach_update_unreach", test_reach_update_unreach},
        {"remove_source_across_heads", test_remove_source_across_heads},
        {"apply_update_stats", test_apply_update_stats},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t fail  = 0;

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
