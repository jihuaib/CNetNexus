/**
 * @file   if_cfg_apply.c
 * @brief  接口配置内存态应用实现（CLI / DB 恢复共用）
 * @author jhb
 * @date   2026/03/08
 */
#include "if_cfg_apply.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <string.h>

#include "db.h"
#include "errcode.h"
#include "if.h"
#include "if_event.h"
#include "if_main.h"
#include "if_pub.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

// ============================================================================
// 公共 API
// ============================================================================

static uint32_t if_cfg_type_to_mask(if_type_t type)
{
    switch (type)
    {
        case IF_TYPE_ETHERNET:
        case IF_TYPE_VETH:
            /* 现阶段统一对外暴露为 ETH 口 */
            return IF_INTF_TYPE_ETH;
        default:
            return 0;
    }
}

static gboolean if_prefix_equal(const net_prefix_t *a, const net_prefix_t *b)
{
    if (!a || !b)
    {
        return FALSE;
    }
    return (a->prefix_len == b->prefix_len && net_addr_equal(&a->addr, &b->addr)) ? TRUE : FALSE;
}

static int if_prefix_to_network(const net_prefix_t *prefix, net_addr_t *out)
{
    if (!prefix || !out || !net_prefix_is_set(prefix))
    {
        return -1;
    }

    if (prefix->addr.family == AF_INET)
    {
        if (prefix->prefix_len > 32)
        {
            return -1;
        }
        uint32_t ip = ntohl(prefix->addr.u.v4.s_addr);
        uint32_t mask = (prefix->prefix_len == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix->prefix_len));
        memset(out, 0, sizeof(*out));
        out->family = AF_INET;
        out->u.v4.s_addr = htonl(ip & mask);
        return 0;
    }

    if (prefix->addr.family == AF_INET6)
    {
        if (prefix->prefix_len > 128)
        {
            return -1;
        }
        memset(out, 0, sizeof(*out));
        out->family = AF_INET6;
        memcpy(out->u.v6.s6_addr, prefix->addr.u.v6.s6_addr, 16);

        uint8_t bits = prefix->prefix_len;
        for (int i = 0; i < 16; i++)
        {
            if (bits >= 8)
            {
                bits -= 8;
                continue;
            }
            if (bits == 0)
            {
                out->u.v6.s6_addr[i] = 0;
            }
            else
            {
                uint8_t mask = (uint8_t)(0xFFu << (8 - bits));
                out->u.v6.s6_addr[i] &= mask;
                bits = 0;
            }
        }
        return 0;
    }

    return -1;
}

static void if_make_zero_addr(sa_family_t family, net_addr_t *out)
{
    if (!out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->family = family;
}

static void if_sync_connected_host_routes(const net_prefix_t *prefix, const char *physical_name, gboolean is_withdraw)
{
    if (!prefix || !net_prefix_is_set(prefix) || !physical_name)
    {
        return;
    }

    uint16_t afi = 0;
    if (prefix->addr.family == AF_INET)
    {
        afi = ROUTE_AFI_IPV4;
    }
    else if (prefix->addr.family == AF_INET6)
    {
        afi = ROUTE_AFI_IPV6;
    }
    else
    {
        return;
    }

    dev_ipc_context_t *ctx = g_if_local ? if_local_ipc_ctx() : NULL;
    if (!ctx)
    {
        return;
    }

    /* 普通接口：注入/撤销直连路由 */
    net_addr_t network_addr;
    if (if_prefix_to_network(prefix, &network_addr) != 0)
    {
        return;
    }

    net_addr_t zero_nh;
    if_make_zero_addr(prefix->addr.family, &zero_nh);

    /* 出接口索引（0 表示接口不存在，OS 路由不下发 OIF） */
    uint32_t out_ifindex = (uint32_t)if_nametoindex(physical_name);

    /* 直连网络路由 */
    if (is_withdraw)
    {
        (void)route_rpc_del(ctx, ROUTE_VRF_DEFAULT, afi, &network_addr, prefix->prefix_len, ROUTE_PROTOCOL_CONNECTED,
                            &prefix->addr, out_ifindex);
    }
    else
    {
        (void)route_rpc_add(ctx, ROUTE_VRF_DEFAULT, afi, &network_addr, prefix->prefix_len, ROUTE_PROTOCOL_CONNECTED,
                            &prefix->addr, &zero_nh, 0, ROUTE_ADMIN_DIST_CONNECTED, out_ifindex);
    }
}

if_map_entry_t *if_cfg_find_entry(const char *logical_name)
{
    if (!logical_name || !g_if_local)
    {
        return NULL;
    }

    if_map_t *map = &g_if_local->interface_map;

    if (!map->all_entries)
    {
        return NULL;
    }

    return (if_map_entry_t *)g_hash_table_lookup(map->all_entries, logical_name);
}

int if_cfg_loop_ensure(uint32_t loop_id)
{
    char name[32];
    snprintf(name, sizeof(name), "loop%u", loop_id);

    if (!g_if_local || !g_if_local->interface_map.all_entries)
    {
        return ERRCODE_FAIL;
    }

    /* 已存在则直接返回 */
    if (g_hash_table_lookup(g_if_local->interface_map.all_entries, name))
    {
        return ERRCODE_SUCCESS;
    }

    /* 创建 OS dummy 接口（若不存在） */
    if (!if_exists(name))
    {
        if (if_create_dummy(name) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("IF: 创建 dummy 接口 %s 失败", name);
            return ERRCODE_FAIL;
        }
        if_set_state(name, 1);
    }

    /* 创建内存条目并插入哈希表（key 和 value 均 g_malloc0，由哈希表负责 g_free） */
    if_map_entry_t *entry = (if_map_entry_t *)g_malloc0(sizeof(if_map_entry_t));
    snprintf(entry->logical_name, sizeof(entry->logical_name), "loop%u", loop_id);
    snprintf(entry->physical_name, sizeof(entry->physical_name), "loop%u", loop_id);
    entry->shutdown = 0;

    g_hash_table_insert(g_if_local->interface_map.all_entries, g_strdup(name), entry);
    LOG_INFO("IF: loop 接口 %s 已创建（内存条目）", name);
    return ERRCODE_SUCCESS;
}

int if_cfg_loop_create(uint32_t loop_id)
{
    char name[32];
    snprintf(name, sizeof(name), "loop%u", loop_id);

    /* 已存在则视为成功（幂等） */
    if (if_cfg_find_entry(name))
    {
        LOG_DEBUG("IF: loop 接口 %s 已存在", name);
        return ERRCODE_SUCCESS;
    }

    if (if_cfg_loop_ensure(loop_id) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    /* 写入 DB */
    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "name", name);
    db_record_set_text(rec, "ip_address", "");
    db_record_set_int(rec, "prefix_len", 0);
    db_record_set_int(rec, "shutdown", 0);
    db_rpc_insert_record(if_local_ipc_ctx(), "if_interface", rec);
    db_record_free(rec);

    LOG_INFO("IF: loop 接口 %s 已创建（内存 + DB）", name);
    return ERRCODE_SUCCESS;
}

int if_cfg_loop_delete(uint32_t loop_id)
{
    char name[32];
    snprintf(name, sizeof(name), "loop%u", loop_id);

    if_map_entry_t *entry = if_cfg_find_entry(name);
    if (!entry)
    {
        LOG_WARN("IF: loop 接口 %s 不存在，忽略删除", name);
        return ERRCODE_FAIL;
    }

    /* 撤销 IP（若已配置） */
    if (net_prefix_is_set(&entry->prefix))
    {
        if_cfg_apply_ip(TRUE, name, NULL);
    }

    /* 删除 OS 接口 */
    if_delete_interface(name);

    /* 从哈希表中移除（会触发 g_free 释放 key 和 value） */
    g_hash_table_remove(g_if_local->interface_map.all_entries, name);

    /* 从 DB 中删除 */
    db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(name)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_rpc_delete(if_local_ipc_ctx(), "if_interface", &filter);
    db_value_free(&cond.value);

    LOG_INFO("IF: loop 接口 %s 已删除", name);
    return ERRCODE_SUCCESS;
}

int if_cfg_apply_ip(gboolean is_no, const char *logical_name, const net_prefix_t *prefix)
{
    if (!logical_name)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_ERROR("IF: Interface %s not found", logical_name);
        return ERRCODE_FAIL;
    }

    net_prefix_t old_prefix = entry->prefix;
    gboolean had_old = net_prefix_is_set(&old_prefix);

    if (is_no)
    {
        if (had_old)
        {
            if_sync_connected_host_routes(&old_prefix, entry->physical_name, TRUE);
        }
        memset(&entry->prefix, 0, sizeof(entry->prefix));
        LOG_INFO("IF: %s IP cleared", logical_name);
        return ERRCODE_SUCCESS;
    }

    if (!prefix || !net_prefix_is_set(prefix))
    {
        return ERRCODE_FAIL;
    }

    char ip_str[64];
    net_addr_to_str(&prefix->addr, ip_str, sizeof(ip_str));

    if (had_old && !if_prefix_equal(&old_prefix, prefix))
    {
        if_sync_connected_host_routes(&old_prefix, entry->physical_name, TRUE);
    }

    entry->prefix = *prefix;
    if (!entry->shutdown)
    {
        if_sync_connected_host_routes(&entry->prefix, entry->physical_name, FALSE);
    }

    LOG_INFO("IF: %s IP=%s/%u configured", logical_name, ip_str, prefix->prefix_len);
    return ERRCODE_SUCCESS;
}

int if_cfg_apply_shutdown(gboolean is_no, const char *logical_name)
{
    if (!logical_name)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_ERROR("IF: Interface %s not found", logical_name);
        return ERRCODE_FAIL;
    }

    /* is_no=TRUE → no shutdown → up；is_no=FALSE → shutdown → down */
    int up = is_no ? 1 : 0;
    int old_shutdown = entry->shutdown;

    if (if_set_state(entry->physical_name, up) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: Failed to set interface %s state", entry->physical_name);
        return ERRCODE_FAIL;
    }

    entry->shutdown = up ? 0 : 1;

    if (old_shutdown != entry->shutdown)
    {
        uint32_t if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
        uint32_t event = up ? IF_EVENT_UP : IF_EVENT_DOWN;
        if (if_type != 0)
        {
            if_pub_notify(g_if_local->subscribers, entry, if_type, event, (uint8_t)up);
        }

        if (net_prefix_is_set(&entry->prefix))
        {
            if_sync_connected_host_routes(&entry->prefix, entry->physical_name, up ? FALSE : TRUE);
        }
    }

    LOG_INFO("IF: %s %s", logical_name, up ? "no shutdown" : "shutdown");
    return ERRCODE_SUCCESS;
}
