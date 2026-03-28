/**
 * @file   route_calc.c
 * @brief  Route 优选模块实现：多协议路径管理距离优选，唯一下发 OS 并通知订阅者
 * @author jhb
 * @date   2026/03/24
 */
#include "route_calc.h"

#include <glib.h>
#include <string.h>

#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_main.h"
#include "route_os.h"
#include "route_pub.h"
#include "route_rib.h"

// ============================================================================
// 辅助：path_key 比较 / 当前已下发路径查找
// ============================================================================

static int path_key_same(const route_path_key_t *a, const route_path_key_t *b)
{
    return a->protocol == b->protocol && net_addr_equal(&a->source, &b->source);
}

/* 查找前缀下当前标记为 OS-installed 的路径（理论上最多一条） */
static route_path_t *find_os_installed_path(route_head_t *head)
{
    if (!head)
    {
        return NULL;
    }

    for (GList *l = head->path_list; l; l = l->next)
    {
        route_path_t *path = (route_path_t *)l->data;
        if (path && (path->flags & ROUTE_PATH_FLAG_OS_INSTALLED))
        {
            return path;
        }
    }
    return NULL;
}

/* 同步前缀下路径的 OS-installed 标记：仅 installed_key 对应路径置位，其余清零 */
static void sync_os_installed_flag(route_head_t *head, const route_path_key_t *installed_key)
{
    if (!head)
    {
        return;
    }
    for (GList *l = head->path_list; l; l = l->next)
    {
        route_path_t *path = (route_path_t *)l->data;
        if (!path)
        {
            continue;
        }
        if (installed_key && path_key_same(&path->key, installed_key))
        {
            path->flags |= ROUTE_PATH_FLAG_OS_INSTALLED;
        }
        else
        {
            path->flags &= ~ROUTE_PATH_FLAG_OS_INSTALLED;
        }
    }
}

// ============================================================================
// 辅助：从 head + path 构建路由条目（OS 下发 + 订阅通知共用）
// ============================================================================

static void build_entry(route_msg_entry_t *e, const route_head_t *head, const route_path_t *path)
{
    memset(e, 0, sizeof(*e));
    e->vrf_id = head->key.vrf_id;
    e->afi = (uint16_t)head->key.afi;
    e->safi = (uint8_t)ROUTE_SAFI_UNICAST;
    e->prefix_len = head->key.prefix_len;
    e->prefix_addr = head->key.addr;
    e->protocol = path->key.protocol;
    e->metric = path->metric;
    e->preference = path->preference;
    e->out_ifindex = path->out_ifindex;
    e->nexthop_addr = path->relay_addr;
    e->source_addr = path->key.source;
    e->is_withdraw = 0;
    e->flags = 0;
}

// ============================================================================
// 辅助：在 path_list 中选最优路径
//   - preference 越小越优先（管理距离）
//   - preference 相等时 metric 越小越优先
//   - skip_key != NULL 时跳过该路径（用于路径删除前的重算场景）
// ============================================================================

static const route_path_t *select_best_path(const route_head_t *head, const route_path_key_t *skip_key)
{
    const route_path_t *best = NULL;
    if (!head)
    {
        return NULL;
    }

    for (GList *l = head->path_list; l; l = l->next)
    {
        const route_path_t *path = (const route_path_t *)l->data;
        if (!path)
        {
            continue;
        }

        /* 跳过待删路径 */
        if (skip_key && path_key_same(&path->key, skip_key))
        {
            continue;
        }

        if (!best || path->preference < best->preference ||
            (path->preference == best->preference && path->metric < best->metric))
        {
            best = path;
        }
    }
    return best;
}

// ============================================================================
// 内部：重算前缀最优路径，同步 OS 并通知订阅者
//
// 通知语义：订阅者只接收"当前最优路径"的变更事件。
//   - 最优路径切换：先发旧路径的 withdraw，再发新路径的 add
//   - 属性更新（nexthop/metric/ifindex 变化）：发新路径的 add（upsert 语义）
//   - 路径消失且无替代：发旧路径的 withdraw
// ============================================================================

static void update_prefix(const route_head_t *head, const route_path_key_t *skip_key)
{
    if (!head)
    {
        return;
    }

    route_head_t *mut_head = (route_head_t *)head;
    GList *subscribers = g_route_local ? g_route_local->subscribers : NULL;

    const route_path_t *new_best = select_best_path(head, skip_key);
    route_path_t *cur_installed = find_os_installed_path(mut_head);

    /* 情形 1：前缀下已无可用路径 */
    if (!new_best)
    {
        if (cur_installed)
        {
            route_msg_entry_t cur_entry;
            char addr_str[64];

            build_entry(&cur_entry, head, cur_installed);
            net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
            LOG_INFO("[route_calc] %s/%u vrf=%u 无可用路径，撤销 OS 及通知: proto=%u", addr_str,
                     (unsigned)head->key.prefix_len, head->key.vrf_id, cur_installed->key.protocol);

            if (route_os_withdraw(&cur_entry) != 0)
            {
                LOG_WARN("[route_calc] OS 撤销失败，保留当前最优状态: %s/%u vrf=%u proto=%u", addr_str,
                         (unsigned)head->key.prefix_len, head->key.vrf_id, cur_installed->key.protocol);
                return;
            }
            if (subscribers)
            {
                cur_entry.is_withdraw = 1;
                route_pub_notify_entry(subscribers, &cur_entry);
            }
        }
        sync_os_installed_flag(mut_head, NULL);
        return;
    }

    route_msg_entry_t new_entry;
    build_entry(&new_entry, head, new_best);

    /* 情形 2：当前已安装的就是最优路径（同协议同来源） */
    if (cur_installed && path_key_same(&cur_installed->key, &new_best->key))
    {
        char addr_str[64];
        net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
        LOG_DEBUG("[route_calc] 最优路径更新: %s/%u vrf=%u proto=%u", addr_str, (unsigned)head->key.prefix_len,
                  head->key.vrf_id, new_best->key.protocol);

        if (route_os_install(&new_entry) != 0)
        {
            LOG_WARN("[route_calc] OS 更新失败，保持旧最优: %s/%u vrf=%u proto=%u", addr_str,
                     (unsigned)head->key.prefix_len, head->key.vrf_id, new_best->key.protocol);
            return;
        }
        if (subscribers)
        {
            route_pub_notify_entry(subscribers, &new_entry);
        }
        route_rib_promote_path_first(mut_head, (route_path_t *)new_best);
        sync_os_installed_flag(mut_head, &new_best->key);
        return;
    }

    /* 情形 3：最优路径切换或首次安装 */
    if (cur_installed)
    {
        route_msg_entry_t cur_entry;
        char addr_str[64];
        build_entry(&cur_entry, head, cur_installed);

        net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
        LOG_INFO("[route_calc] 最优路径切换: %s/%u vrf=%u proto=%u(pref=%d) -> proto=%u(pref=%d)", addr_str,
                 (unsigned)head->key.prefix_len, head->key.vrf_id, cur_installed->key.protocol, cur_entry.preference,
                 new_best->key.protocol, new_best->preference);

        if (route_os_install(&new_entry) != 0)
        {
            LOG_WARN("[route_calc] OS 安装失败，保持当前最优不变: %s/%u vrf=%u proto=%u", addr_str,
                     (unsigned)head->key.prefix_len, head->key.vrf_id, new_best->key.protocol);
            return;
        }
        /* 不再显式撤销旧最优：当前下发不携带 RTA_PRIORITY，REPLACE 语义由内核覆盖同前缀主项。 */

        if (subscribers)
        {
            cur_entry.is_withdraw = 1;
            route_pub_notify_entry(subscribers, &cur_entry);
            route_pub_notify_entry(subscribers, &new_entry);
        }
    }
    else
    {
        char addr_str[64];
        net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
        LOG_INFO("[route_calc] 安装最优路由: %s/%u vrf=%u proto=%u pref=%d metric=%d", addr_str,
                 (unsigned)head->key.prefix_len, head->key.vrf_id, new_best->key.protocol, new_best->preference,
                 new_best->metric);

        if (route_os_install(&new_entry) != 0)
        {
            LOG_WARN("[route_calc] OS 安装失败，保持当前最优不变: %s/%u vrf=%u proto=%u", addr_str,
                     (unsigned)head->key.prefix_len, head->key.vrf_id, new_best->key.protocol);
            return;
        }

        if (subscribers)
        {
            route_pub_notify_entry(subscribers, &new_entry);
        }
    }

    route_rib_promote_path_first(mut_head, (route_path_t *)new_best);
    sync_os_installed_flag(mut_head, &new_best->key);
}

// ============================================================================
// 生命周期
// ============================================================================

void route_calc_init(void)
{
    LOG_INFO("[route_calc] 优选状态初始化完成（基于 route_path OS 标记）");
}

void route_calc_cleanup(void)
{
    LOG_INFO("[route_calc] 优选状态已清理");
}

// ============================================================================
// 公共接口：RIB 变更通知
// ============================================================================

void route_calc_on_path_add(const route_head_t *head)
{
    update_prefix(head, NULL);
}

void route_calc_on_path_del(const route_head_t *head, const route_path_t *del_path)
{
    if (!del_path)
    {
        return;
    }
    update_prefix(head, &del_path->key);
}

// ============================================================================
// 公共接口：全量最优路径快照（用于 SUBSCRIBE+FULL 响应）
// ============================================================================

typedef struct
{
    route_msg_entry_t *entries; /**< 收集到的条目数组 */
    uint32_t count;             /**< 已收集条目数 */
    uint32_t capacity;          /**< 数组容量 */
} dump_collect_ctx_t;

static void dump_collect_best(const route_head_t *head, const route_path_t *path, void *userdata)
{
    dump_collect_ctx_t *dctx = (dump_collect_ctx_t *)userdata;
    if (!head || !path || !dctx)
    {
        return;
    }

    if (!(path->flags & ROUTE_PATH_FLAG_OS_INSTALLED))
    {
        return;
    }

    if (dctx->count >= dctx->capacity)
    {
        uint32_t new_cap = dctx->capacity ? dctx->capacity * 2u : 64u;
        route_msg_entry_t *new_buf = (route_msg_entry_t *)g_realloc(dctx->entries, new_cap * sizeof(route_msg_entry_t));
        if (!new_buf)
        {
            return;
        }
        dctx->entries = new_buf;
        dctx->capacity = new_cap;
    }

    build_entry(&dctx->entries[dctx->count], head, path);
    dctx->entries[dctx->count].is_withdraw = 0;
    dctx->count++;
}

void route_calc_pub_dump(uint32_t dst_module_id, uint32_t protocol, uint32_t vrf_id, uint32_t request_id)
{
    if (!g_route_local || !g_route_local->rib)
    {
        return;
    }
    dev_ipc_context_t *pub_ctx = route_local_ipc_ctx();

    dump_collect_ctx_t dctx = {
        .entries = NULL,
        .count = 0,
        .capacity = 0,
    };
    route_rib_walk(g_route_local->rib, protocol, vrf_id, dump_collect_best, &dctx);

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

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_REPORT, DEV_MODULE_ID_ROUTE, dst_module_id,
                                                    request_id, report, (uint32_t)report_size, g_free);
    if (!msg)
    {
        g_free(report);
        return;
    }

    if (dev_ipc_send_response(pub_ctx, msg) != ERRCODE_SUCCESS)
    {
        LOG_WARN("[route_calc] 全量快照发送失败 -> module 0x%08X", dst_module_id);
    }
    dev_ipc_message_free(msg);

    LOG_INFO("[route_calc] 全量最优路径快照: %u 条 -> module 0x%08X", dctx.count, dst_module_id);
}
