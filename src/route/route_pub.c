/**
 * @file   route_pub.c
 * @brief  Route 模块发布/subscribe实现
 * @author jhb
 * @date   2026/02/01
 */
#include "route_pub.h"

#include <stdlib.h>
#include <string.h>

#include "errcode.h"
#include "log.h"
#include "route.h"

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 将 route_head_t + route_path_t 转换为 route_msg_entry_t（直接二进制拷贝）
 */
static void build_entry(route_msg_entry_t *entry, const route_head_t *head, const route_path_t *path, int is_withdraw)
{
    memset(entry, 0, sizeof(*entry));
    entry->vrf_id = head->key.vrf_id;
    entry->afi = head->key.afi;
    entry->safi = ROUTE_SAFI_UNICAST;
    entry->prefix_len = head->key.prefix_len;
    entry->protocol = path->key.protocol;
    entry->metric = path->metric;
    entry->preference = path->preference;
    entry->is_withdraw = (uint8_t)is_withdraw;
    if (path->iter_required)
    {
        entry->flags |= ROUTE_ENTRY_FLAG_REQUIRE_NH_ITER;
    }
    entry->prefix_addr = head->key.addr;
    entry->nexthop_addr = path->nexthop;
    entry->source_addr = path->key.source;
}

// ============================================================================
// subscribe者匹配
// ============================================================================

static int subscriber_matches(const route_subscriber_t *sub, const route_head_t *head, const route_path_t *path)
{
    /* 协议过滤 */
    if (sub->protocol != ROUTE_PROTOCOL_MAX && sub->protocol != path->key.protocol)
    {
        return 0;
    }
    /* VRF 过滤 */
    if (sub->vrf_id != ROUTE_VRF_ALL && sub->vrf_id != head->key.vrf_id)
    {
        return 0;
    }
    return 1;
}

// ============================================================================
// 公共 API
// ============================================================================

void route_pub_notify_module(dev_ipc_context_t *ctx, uint32_t dst_module_id, const route_head_t *head,
                             const route_path_t *path, int is_withdraw)
{
    if (!ctx || !head || !path || dst_module_id == 0)
    {
        return;
    }

    route_msg_entry_t *payload = (route_msg_entry_t *)g_malloc(sizeof(route_msg_entry_t));
    if (!payload)
    {
        return;
    }
    build_entry(payload, head, path, is_withdraw);

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_UPDATE, DEV_MODULE_ID_ROUTE, dst_module_id, 0,
                                                    payload, sizeof(route_msg_entry_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return;
    }

    if (dev_ipc_send(ctx, dst_module_id, msg) != 0)
    {
        LOG_WARN("Failed to send route update to module 0x%08X", dst_module_id);
    }
    dev_ipc_message_free(msg);
}

void route_pub_notify(dev_ipc_context_t *ctx, GList *subscribers, const route_head_t *head, const route_path_t *path,
                      int is_withdraw)
{
    if (!ctx || !head || !path)
    {
        return;
    }

    for (GList *l = subscribers; l; l = l->next)
    {
        route_subscriber_t *sub = (route_subscriber_t *)l->data;
        if (!subscriber_matches(sub, head, path))
        {
            continue;
        }

        route_pub_notify_module(ctx, sub->module_id, head, path, is_withdraw);
    }
}

// ============================================================================
// 全量转储辅助
// ============================================================================

typedef struct
{
    route_msg_entry_t *entries;
    uint32_t count;
    uint32_t capacity;
} dump_ctx_t;

static void collect_path(const route_head_t *head, const route_path_t *path, void *userdata)
{
    dump_ctx_t *ctx = (dump_ctx_t *)userdata;

    /* 迭代型路径走 owner 回推通道，不进入公共订阅快照 */
    if (path->iter_required)
    {
        return;
    }

    if (ctx->count >= ctx->capacity)
    {
        uint32_t new_cap = ctx->capacity ? ctx->capacity * 2 : 64;
        route_msg_entry_t *new_entries =
            (route_msg_entry_t *)g_realloc(ctx->entries, new_cap * sizeof(route_msg_entry_t));
        if (!new_entries)
        {
            return;
        }
        ctx->entries = new_entries;
        ctx->capacity = new_cap;
    }

    build_entry(&ctx->entries[ctx->count], head, path, 0);
    ctx->count++;
}

void route_pub_dump(dev_ipc_context_t *ctx, route_rib_t *rib, uint32_t dst_module_id, uint32_t protocol,
                    uint32_t vrf_id, uint32_t request_id)
{
    if (!ctx || !rib)
    {
        return;
    }

    /* 收集所有匹配路径 */
    dump_ctx_t dctx = {NULL, 0, 0};
    route_rib_walk(rib, protocol, vrf_id, collect_path, &dctx);

    /* 构建 route_msg_report_t */
    size_t report_size = sizeof(route_msg_report_t) + dctx.count * sizeof(route_msg_entry_t);
    route_msg_report_t *report = (route_msg_report_t *)g_malloc(report_size);
    if (!report)
    {
        g_free(dctx.entries);
        return;
    }

    report->protocol = protocol;
    report->route_count = dctx.count;
    if (dctx.count > 0)
    {
        memcpy(report->routes, dctx.entries, dctx.count * sizeof(route_msg_entry_t));
    }
    g_free(dctx.entries);

    /* 作为响应发送给请求方 */
    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_REPORT, DEV_MODULE_ID_ROUTE, dst_module_id,
                                                    request_id, report, (uint32_t)report_size, g_free);
    if (!msg)
    {
        g_free(report);
        return;
    }

    if (dev_ipc_send_response(ctx, msg) != 0)
    {
        LOG_WARN("Failed to send full route report to module 0x%08X", dst_module_id);
    }
    dev_ipc_message_free(msg);

    LOG_INFO("Full route report: %u routes -> module 0x%08X", dctx.count, dst_module_id);
}
