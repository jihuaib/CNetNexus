/**
 * @file   bgp_pub.c
 * @brief  BGP 路由发布链表实现
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_pub.h"

#include <string.h>

#include "bgp_pkt.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "log.h"

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 发送上下文：传递给 g_hash_table_foreach 的参数包
 *
 * peer_hash 的键为 net_addr_t*，通过 bgp_vrf_find_session 找到对应会话。
 */
typedef struct
{
    bgp_instance_t *inst;
    const bgp_pub_entry_t *entry;
} pub_send_ctx_t;

/**
 * @brief 释放单条发布路由条目
 *
 * 当前所有字段均为值语义（固定数组），g_free 足以完成清理。
 * 若将来在 bgp_pub_entry_t 中加入堆指针字段，在此统一释放。
 */
static void pub_entry_free(bgp_pub_entry_t *entry)
{
    if (!entry)
    {
        return;
    }
    /* TODO: 若将来添加堆指针字段，在此 g_free 后再释放 */
    g_free(entry);
}

/** 按 NLRI key 查找链表节点，未找到返回 NULL */
static GList *publist_find_node(const bgp_publist_t *list, const bgp_nlri_entry_t *nlri)
{
    for (GList *node = list->entries; node; node = node->next)
    {
        const bgp_pub_entry_t *entry = node->data;
        if (strcmp(entry->nlri.key, nlri->key) == 0)
        {
            return node;
        }
    }
    return NULL;
}

/** g_hash_table_foreach 回调：遍历 inst->peer_hash，向各对端对应会话发 UPDATE */
static void foreach_send_update(gpointer key, gpointer value, gpointer user_data)
{
    (void)value; /* bgp_peer_t*，此处仅需地址 */
    const net_addr_t *addr = key;
    pub_send_ctx_t *ctx = user_data;

    bgp_session_t *sess = bgp_vrf_find_session(ctx->inst->vrf, addr);
    if (!sess || sess->state != BGP_CONN_STATE_ESTABLISHED || !sess->pri_conn || sess->pri_conn->fd < 0)
    {
        return;
    }
    bgp_pkt_send_update(sess->pri_conn, &ctx->entry->nlri, &ctx->entry->attr, &ctx->entry->nexthop);
}

/** g_hash_table_foreach 回调：遍历 inst->peer_hash，向各对端对应会话发 WITHDRAW */
static void foreach_send_withdraw(gpointer key, gpointer value, gpointer user_data)
{
    (void)value;
    const net_addr_t *addr = key;
    pub_send_ctx_t *ctx = user_data;

    bgp_session_t *sess = bgp_vrf_find_session(ctx->inst->vrf, addr);
    if (!sess || sess->state != BGP_CONN_STATE_ESTABLISHED || !sess->pri_conn || sess->pri_conn->fd < 0)
    {
        return;
    }
    bgp_pkt_send_withdraw(sess->pri_conn, &ctx->entry->nlri);
}

// ============================================================================
// 链表生命周期
// ============================================================================

bgp_publist_t *bgp_publist_create(bgp_instance_t *inst)
{
    bgp_publist_t *list = g_malloc0(sizeof(bgp_publist_t));
    list->inst = inst;
    return list;
}

void bgp_publist_destroy(bgp_publist_t *list)
{
    if (!list)
    {
        return;
    }
    for (GList *node = list->entries; node; node = node->next)
    {
        pub_entry_free(node->data);
    }
    g_list_free(list->entries);
    g_free(list);
}

// ============================================================================
// 路由操作
// ============================================================================

int bgp_publist_add(bgp_publist_t *list, const bgp_nlri_entry_t *nlri, const bgp_attr_t *attr,
                    const bgp_nexthop_t *nexthop)
{
    if (!list || !nlri || !attr || !nexthop)
    {
        return -1;
    }

    bgp_pub_entry_t *entry;
    GList *node = publist_find_node(list, nlri);

    if (node)
    {
        /* 已存在：就地更新属性和下一跳，然后重新宣告 */
        entry = node->data;
        entry->attr = *attr;
        entry->nexthop = *nexthop;
        LOG_DEBUG("BGP: publist 更新路由 key=%s", nlri->key);
    }
    else
    {
        /* 新增条目 */
        entry = g_malloc0(sizeof(bgp_pub_entry_t));
        entry->nlri = *nlri;
        entry->attr = *attr;
        entry->nexthop = *nexthop;
        list->entries = g_list_append(list->entries, entry);
        list->count++;
        LOG_DEBUG("BGP: publist 新增路由 key=%s count=%u", nlri->key, list->count);
    }

    /* 向该 AF 实例所有 ESTABLISHED 会话宣告 */
    if (list->inst && list->inst->peer_hash)
    {
        pub_send_ctx_t ctx = {.inst = list->inst, .entry = entry};
        g_hash_table_foreach(list->inst->peer_hash, foreach_send_update, &ctx);
    }
    return 0;
}

int bgp_publist_del(bgp_publist_t *list, const bgp_nlri_entry_t *nlri)
{
    if (!list || !nlri)
    {
        return -1;
    }

    GList *node = publist_find_node(list, nlri);
    if (!node)
    {
        return -1;
    }

    bgp_pub_entry_t *entry = node->data;

    /* 先向所有 ESTABLISHED 会话发送 WITHDRAW */
    if (list->inst && list->inst->peer_hash)
    {
        pub_send_ctx_t ctx = {.inst = list->inst, .entry = entry};
        g_hash_table_foreach(list->inst->peer_hash, foreach_send_withdraw, &ctx);
    }

    list->entries = g_list_delete_link(list->entries, node);
    list->count--;
    LOG_DEBUG("BGP: publist 删除路由 key=%s count=%u", nlri->key, list->count);
    pub_entry_free(entry);
    return 0;
}

// ============================================================================
// 连接级操作（会话建立/关闭时调用）
// ============================================================================

void bgp_publist_announce_to_conn(const bgp_publist_t *list, bgp_conn_t *conn)
{
    if (!list || !conn || conn->fd < 0)
    {
        return;
    }
    for (GList *node = list->entries; node; node = node->next)
    {
        const bgp_pub_entry_t *entry = node->data;
        bgp_pkt_send_update(conn, &entry->nlri, &entry->attr, &entry->nexthop);
    }
    LOG_DEBUG("BGP: publist 已向 fd=%d 宣告 %u 条路由", conn->fd, list->count);
}

void bgp_publist_withdraw_from_conn(const bgp_publist_t *list, bgp_conn_t *conn)
{
    if (!list || !conn || conn->fd < 0)
    {
        return;
    }
    for (GList *node = list->entries; node; node = node->next)
    {
        const bgp_pub_entry_t *entry = node->data;
        bgp_pkt_send_withdraw(conn, &entry->nlri);
    }
    LOG_DEBUG("BGP: publist 已向 fd=%d 撤销 %u 条路由", conn->fd, list->count);
}
