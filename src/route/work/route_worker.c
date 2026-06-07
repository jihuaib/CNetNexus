/**
 * @file   route_worker.c
 * @brief  Route worker 线程实现：epoll 事件循环与业务数据处理
 * @author jhb
 * @date   2026/03/28
 */
#include "route_worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cli.h"
#include "errcode.h"
#include "fib.h"
#include "if.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_bdr.h"
#include "route_calc.h"
#include "route_cfg_apply.h"
#include "route_main.h"
#include "route_nhobj.h"
#include "route_pub.h"
#include "route_relay.h"
#include "route_rib.h"
#include "route_show.h"
#include "route_static.h"
#include "route_work.h"
#include "vrf.h"

/** worker epoll 单次最大事件数 */
#define ROUTE_MAX_EPOLL_EVENTS 8

route_work_local_t *g_route_work_local = NULL;

/** cmd eventfd 哨兵标签（地址用于 epoll data.ptr 精确比较） */
static char g_route_cmd_tag;
/** work eventfd 哨兵标签（地址用于 epoll data.ptr 精确比较） */
static char g_route_work_tag;

// ============================================================================
// 内部命令队列结构（私有）
// ============================================================================

/**
 * @brief worker 内部命令结构体（IPC 线程分配，worker 线程处理后释放）
 */
typedef struct route_worker_cmd
{
    route_worker_cmd_type_t type; /**< 命令类型 */
    dev_ipc_message_t *msg;       /**< 关联 IPC 消息（可为 NULL） */
    route_apply_cmd_t *apply;     /**< APPLY 命令参数（借用引用，不持有所有权） */
    char vrf_name[VRF_NAME_MAX_LEN];
    uint32_t *vrf_id_out;
    int waitable;          /**< 是否需要同步等待结果（APPLY 命令为 1） */
    pthread_mutex_t mutex; /**< 同步等待互斥锁 */
    pthread_cond_t cond;   /**< 同步等待条件变量 */
    int done;              /**< worker 是否已完成处理 */
    int rc;                /**< worker 处理结果 */
} route_worker_cmd_t;

typedef enum route_worker_event_type
{
    ROUTE_WORK_EVENT_CALC = 1, /**< 触发一条前缀的优选处理 */
} route_worker_event_type_t;

typedef struct route_worker_event
{
    route_worker_event_type_t type;
    union
    {
        route_head_key_t key;
    } u;
} route_worker_event_t;

static route_worker_cmd_t *worker_cmd_create(route_worker_cmd_type_t type, dev_ipc_message_t *msg, int waitable)
{
    route_worker_cmd_t *cmd = (route_worker_cmd_t *)g_malloc0(sizeof(route_worker_cmd_t));
    if (!cmd)
    {
        return NULL;
    }
    cmd->type = type;
    cmd->msg = msg;
    cmd->waitable = waitable;
    if (waitable)
    {
        pthread_mutex_init(&cmd->mutex, NULL);
        pthread_cond_init(&cmd->cond, NULL);
    }
    return cmd;
}

static void worker_cmd_destroy(route_worker_cmd_t *cmd)
{
    if (!cmd)
    {
        return;
    }
    if (cmd->waitable)
    {
        pthread_mutex_destroy(&cmd->mutex);
        pthread_cond_destroy(&cmd->cond);
    }
    g_free(cmd);
}

static void worker_cmd_complete(route_worker_cmd_t *cmd, int rc)
{
    if (!cmd || !cmd->waitable)
    {
        return;
    }
    pthread_mutex_lock(&cmd->mutex);
    cmd->rc = rc;
    cmd->done = 1;
    pthread_cond_signal(&cmd->cond);
    pthread_mutex_unlock(&cmd->mutex);
}

static int worker_cmd_wait(route_worker_cmd_t *cmd)
{
    if (!cmd || !cmd->waitable)
    {
        return ERRCODE_FAIL;
    }
    pthread_mutex_lock(&cmd->mutex);
    while (!cmd->done)
    {
        pthread_cond_wait(&cmd->cond, &cmd->mutex);
    }
    int rc = cmd->rc;
    pthread_mutex_unlock(&cmd->mutex);
    return rc;
}

static route_worker_event_t *worker_event_create_calc(const route_head_key_t *key)
{
    if (!key)
    {
        return NULL;
    }

    route_worker_event_t *evt = (route_worker_event_t *)g_malloc(sizeof(*evt));
    if (!evt)
    {
        return NULL;
    }

    evt->type = ROUTE_WORK_EVENT_CALC;
    evt->u.key = *key;
    return evt;
}

static void worker_event_destroy(route_worker_event_t *evt)
{
    if (!evt)
    {
        return;
    }
    g_free(evt);
}

static void worker_signal_cmd_event(void)
{
    if (!g_route_work_local || g_route_work_local->cmd_eventfd < 0)
    {
        return;
    }

    uint64_t one = 1;
    if (write(g_route_work_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("[route_worker] write cmd_eventfd 失败");
        }
    }
}

static void worker_signal_work_event(void)
{
    if (!g_route_work_local || g_route_work_local->work_eventfd < 0)
    {
        return;
    }

    uint64_t one = 1;
    if (write(g_route_work_local->work_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("[route_worker] write work_eventfd 失败");
        }
    }
}

static int worker_cmd_enqueue(route_worker_cmd_t *cmd)
{
    if (!cmd || !g_route_work_local || !g_route_work_local->cmd_queue || g_route_work_local->cmd_eventfd < 0)
    {
        return -1;
    }
    g_async_queue_push(g_route_work_local->cmd_queue, cmd);
    worker_signal_cmd_event();
    return 0;
}

static int worker_event_enqueue(route_worker_event_t *evt)
{
    if (!evt || !g_route_work_local || !g_route_work_local->work_queue || g_route_work_local->work_eventfd < 0)
    {
        return -1;
    }

    g_async_queue_push(g_route_work_local->work_queue, evt);
    worker_signal_work_event();
    return 0;
}

static int route_worker_post_calc_event(const route_head_key_t *key)
{
    route_worker_event_t *evt = worker_event_create_calc(key);
    if (!evt)
    {
        return -1;
    }
    if (worker_event_enqueue(evt) != 0)
    {
        worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

static void on_inject_path_del(const route_head_t *head, const route_path_t *path, void *userdata);

// ============================================================================
// 本地回环路由：VRF 创建时自动下发 127.0.0.0/8、127.0.0.1/32 和 ::1/128
// ============================================================================

static void worker_make_ipv4_addr(net_addr_t *addr, uint32_t host_order)
{
    memset(addr, 0, sizeof(*addr));
    addr->family = AF_INET;
    addr->u.v4.s_addr = htonl(host_order);
}

static void worker_make_ipv6_loopback_addr(net_addr_t *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->family = AF_INET6;
    addr->u.v6.s6_addr[15] = 1u;
}

static int worker_install_loopback_route(uint32_t vrf_id, uint32_t prefix_host_order, uint8_t prefix_len)
{
    net_addr_t nh_addr;
    net_addr_t prefix_addr;
    worker_make_ipv4_addr(&nh_addr, 0x7F000001u);
    worker_make_ipv4_addr(&prefix_addr, prefix_host_order);

    uint32_t entry_flags = ROUTE_ENTRY_FLAG_NO_ADV | ROUTE_ENTRY_FLAG_LOCAL;
    int ret = route_rib_add(g_route_work_local->rib, vrf_id, ROUTE_AFI_IPV4, &prefix_addr, prefix_len,
                            ROUTE_PROTOCOL_CONNECTED, &nh_addr, &nh_addr, 0, ROUTE_ADMIN_DIST_CONNECTED,
                            ROUTE_INLOOP_IFINDEX, ROUTE_NH_TYPE_IP, 0u, 0u, entry_flags);
    if (ret < 0)
    {
        return ret;
    }

    const route_head_t *head =
        route_rib_lookup_head(g_route_work_local->rib, vrf_id, ROUTE_AFI_IPV4, &prefix_addr, prefix_len);
    if (head)
    {
        const route_path_t *path = route_rib_lookup_path(head, ROUTE_PROTOCOL_CONNECTED, &nh_addr);
        if (path)
        {
            route_nhobj_set_relay(path->nexthop_id, &nh_addr, ROUTE_INLOOP_IFINDEX);
        }
        route_work_handle_calc_event(&head->key);
    }

    return ret;
}

static int worker_install_loopback_route_v6(uint32_t vrf_id)
{
    net_addr_t nh_addr;
    net_addr_t prefix_addr;
    worker_make_ipv6_loopback_addr(&nh_addr);
    worker_make_ipv6_loopback_addr(&prefix_addr);

    uint32_t entry_flags = ROUTE_ENTRY_FLAG_NO_ADV | ROUTE_ENTRY_FLAG_LOCAL;
    int ret = route_rib_add(g_route_work_local->rib, vrf_id, ROUTE_AFI_IPV6, &prefix_addr, 128,
                            ROUTE_PROTOCOL_CONNECTED, &nh_addr, &nh_addr, 0, ROUTE_ADMIN_DIST_CONNECTED,
                            ROUTE_INLOOP_IFINDEX, ROUTE_NH_TYPE_IP, 0u, 0u, entry_flags);
    if (ret < 0)
    {
        return ret;
    }

    const route_head_t *head =
        route_rib_lookup_head(g_route_work_local->rib, vrf_id, ROUTE_AFI_IPV6, &prefix_addr, 128);
    if (head)
    {
        const route_path_t *path = route_rib_lookup_path(head, ROUTE_PROTOCOL_CONNECTED, &nh_addr);
        if (path)
        {
            route_nhobj_set_relay(path->nexthop_id, &nh_addr, ROUTE_INLOOP_IFINDEX);
        }
        route_work_handle_calc_event(&head->key);
    }

    return ret;
}

/**
 * @brief 为指定 VRF 安装本地回环路由
 *
 * 下发三条路由：
 *   - 127.0.0.0/8   nexthop 127.0.0.1  出接口 inloop0
 *   - 127.0.0.1/32  nexthop 127.0.0.1  出接口 inloop0
 *   - ::1/128       nexthop ::1        出接口 inloop0
 *
 * 使用 ROUTE_PROTOCOL_CONNECTED 协议类型，source_addr 设为 localhost 地址作为路径标识。
 * ROUTE_ENTRY_FLAG_LOCAL 表示 FIB 对非 public VRF 下发 RTN_LOCAL；public 只保留内部视图。
 */
static void worker_install_loopback_routes(uint32_t vrf_id)
{
    if (!g_route_work_local || !g_route_work_local->rib)
    {
        return;
    }

    int ret1 = worker_install_loopback_route(vrf_id, 0x7F000000u, 8);
    int ret2 = worker_install_loopback_route(vrf_id, 0x7F000001u, 32);
    int ret3 = worker_install_loopback_route_v6(vrf_id);

    LOG_INFO("[route_worker] installed loopback routes for vrf_id=%u (127.0.0.0/8 rc=%d, 127.0.0.1/32 rc=%d, "
             "::1/128 rc=%d)",
             vrf_id, ret1, ret2, ret3);
}

/**
 * @brief 撤销指定 VRF 的本地回环路由
 *
 * 在 VRF 删除时调用，撤销 127.0.0.0/8、127.0.0.1/32 和 ::1/128 三条路由。
 */
static void worker_withdraw_loopback_routes(uint32_t vrf_id)
{
    if (!g_route_work_local || !g_route_work_local->rib)
    {
        return;
    }

    net_addr_t source;
    worker_make_ipv4_addr(&source, 0x7F000001u);

    net_addr_t prefix_8;
    worker_make_ipv4_addr(&prefix_8, 0x7F000000u);

    int del1 = route_rib_del(g_route_work_local->rib, vrf_id, ROUTE_AFI_IPV4, &prefix_8, 8, ROUTE_PROTOCOL_CONNECTED,
                             &source, on_inject_path_del, NULL);

    net_addr_t prefix_32;
    worker_make_ipv4_addr(&prefix_32, 0x7F000001u);

    int del2 = route_rib_del(g_route_work_local->rib, vrf_id, ROUTE_AFI_IPV4, &prefix_32, 32, ROUTE_PROTOCOL_CONNECTED,
                             &source, on_inject_path_del, NULL);

    net_addr_t source_v6;
    worker_make_ipv6_loopback_addr(&source_v6);

    net_addr_t prefix_v6;
    worker_make_ipv6_loopback_addr(&prefix_v6);

    int del3 = route_rib_del(g_route_work_local->rib, vrf_id, ROUTE_AFI_IPV6, &prefix_v6, 128, ROUTE_PROTOCOL_CONNECTED,
                             &source_v6, on_inject_path_del, NULL);

    LOG_INFO("[route_worker] withdrew loopback routes for vrf_id=%u (127.0.0.0/8 rc=%d, 127.0.0.1/32 rc=%d, "
             "::1/128 rc=%d)",
             vrf_id, del1, del2, del3);
}

/* SMOOTHSTART 时回调：收集 cache 内所有非 public VRF id，拆除其在 RIB 中的 VRF 相关业务。
 * 注意只清内存，不动 DB（DB 是后续 SMOOTHEND 重恢复的依据）。 */
static gboolean purge_collect_cb(const vrf_api_cache_entry_t *entry, void *user_data)
{
    GArray *vrf_ids = (GArray *)user_data;
    if (entry && entry->vrf_id != VRF_PUBLIC_VRF_ID)
    {
        g_array_append_val(vrf_ids, entry->vrf_id);
    }
    return FALSE; /* 继续遍历 */
}

void route_worker_purge_non_public_vrf_business(void)
{
    GArray *vrf_ids = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    vrf_api_cache_foreach(purge_collect_cb, vrf_ids);

    for (guint i = 0; i < vrf_ids->len; i++)
    {
        uint32_t vid = g_array_index(vrf_ids, uint32_t, i);
        worker_withdraw_loopback_routes(vid);
        int rc = route_static_del_vrf(vid);
        LOG_INFO("Route resync: purged loopback + %d static path(s) for vrf_id=%u", rc > 0 ? rc : 0, vid);
    }
    g_array_free(vrf_ids, TRUE);
}

static int worker_resolve_vrf_id_by_name(const char *vrf_name, uint32_t *vrf_id)
{
    if (!vrf_id)
    {
        return ERRCODE_FAIL;
    }

    *vrf_id = ROUTE_VRF_DEFAULT;
    if (!vrf_name || vrf_name[0] == '\0' || strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        return ERRCODE_SUCCESS;
    }

    const vrf_api_cache_entry_t *vrf = vrf_api_cache_lookup_by_name(vrf_name);
    if (!vrf)
    {
        return ERRCODE_DEP_MISSING;
    }

    *vrf_id = vrf->vrf_id;
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 辅助：路径删除前回调（由 route_rib_del 在删除前触发）
// ============================================================================

/**
 * @brief route_rib_del 回调：在路径从 RIB 移除前同步触发优选重算
 *
 * 此时 del_path 仍在 RIB 中，route_calc_on_path_del 可跳过它选次优路径。
 */
static void on_inject_path_del(const route_head_t *head, const route_path_t *path, void *userdata)
{
    (void)userdata;
    route_calc_on_path_del(head, path);
}

static void worker_send_inject_ack(const dev_ipc_message_t *req, int32_t result)
{
    if (!req || req->request_id == 0)
    {
        return;
    }

    route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc0(sizeof(route_msg_ack_t));
    if (!ack)
    {
        return;
    }
    ack->result = result;

    dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, req->src_module_id,
                                                     req->request_id, ack, sizeof(route_msg_ack_t), g_free);
    if (!resp)
    {
        g_free(ack);
        return;
    }

    dev_ipc_send_response(route_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
}

static void worker_send_nhobj_ack(const dev_ipc_message_t *req, int32_t result, uint32_t nexthop_id)
{
    if (!req || req->request_id == 0)
    {
        return;
    }

    route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc0(sizeof(route_msg_ack_t));
    if (!ack)
    {
        return;
    }
    ack->result = result;
    ack->nexthop_id = nexthop_id;

    dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, req->src_module_id,
                                                     req->request_id, ack, sizeof(route_msg_ack_t), g_free);
    if (!resp)
    {
        g_free(ack);
        return;
    }

    dev_ipc_send_response(route_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
}

static void worker_handle_nhobj_acquire(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(route_nhobj_msg_t))
    {
        worker_send_nhobj_ack(msg, ERRCODE_FAIL, 0u);
        if (msg)
        {
            dev_ipc_message_free(msg);
        }
        return;
    }

    const route_nhobj_msg_t *req = (const route_nhobj_msg_t *)msg->payload;
    uint32_t nexthop_id = 0u;
    /* req->nexthop_id 非 0 表示业务进程重启反刷，要求按原 id 恢复对象 */
    int rc = route_nhobj_acquire(&req->key, req->nexthop_id, &nexthop_id);
    if (rc == ERRCODE_SUCCESS)
    {
        route_nhobj_set_relay(nexthop_id, &req->relay_addr, req->relay_ifindex);
    }

    worker_send_nhobj_ack(msg, (rc == ERRCODE_SUCCESS) ? ERRCODE_SUCCESS : ERRCODE_FAIL, nexthop_id);
    dev_ipc_message_free(msg);
}

static void worker_handle_nhobj_release(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(route_nhobj_release_req_t))
    {
        if (msg)
        {
            dev_ipc_message_free(msg);
        }
        return;
    }

    const route_nhobj_release_req_t *req = (const route_nhobj_release_req_t *)msg->payload;
    route_nhobj_release(req->nexthop_id);
    dev_ipc_message_free(msg);
}

// ============================================================================
// 命令处理：业务逻辑（在 worker 线程运行）
// ============================================================================

static void worker_handle_inject(dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_msg_entry_t))
    {
        LOG_WARN("[route_worker] INJECT payload 太短: %u", msg->payload_len);
        worker_send_inject_ack(msg, ERRCODE_FAIL);
        dev_ipc_message_free(msg);
        return;
    }

    const route_msg_entry_t *entry = (const route_msg_entry_t *)msg->payload;
    int ret = -1;

    if (entry->is_withdraw)
    {
        ret = route_rib_del(g_route_work_local->rib, entry->vrf_id, entry->afi, &entry->prefix_addr, entry->prefix_len,
                            entry->protocol, &entry->source_addr, on_inject_path_del, NULL);
        LOG_DEBUG("[route_worker] INJECT withdraw: vrf=%u afi=%u pfxlen=%u proto=%u ret=%d", entry->vrf_id, entry->afi,
                  entry->prefix_len, entry->protocol, ret);
        route_recompute_iter_paths();
    }
    else
    {
        if (entry->nexthop_id != 0u)
        {
            ret = route_rib_add_nexthop_id(g_route_work_local->rib, entry->vrf_id, entry->afi, &entry->prefix_addr,
                                           entry->prefix_len, entry->protocol, &entry->source_addr, entry->nexthop_id,
                                           entry->metric, entry->preference, entry->out_ifindex, entry->nh_type,
                                           entry->tunnel_id, entry->out_label, (uint32_t)entry->flags);
        }
        else
        {
            ret = route_rib_add(g_route_work_local->rib, entry->vrf_id, entry->afi, &entry->prefix_addr,
                                entry->prefix_len, entry->protocol, &entry->source_addr, &entry->nexthop_addr,
                                entry->metric, entry->preference, entry->out_ifindex, entry->nh_type, entry->tunnel_id,
                                entry->out_label, (uint32_t)entry->flags);
        }
        if (ret >= 0)
        {
            const route_head_t *head = route_rib_lookup_head(g_route_work_local->rib, entry->vrf_id, entry->afi,
                                                             &entry->prefix_addr, entry->prefix_len);
            if (head)
            {
                const route_path_t *path = route_rib_lookup_path(head, entry->protocol, &entry->source_addr);
                if (path && entry->nexthop_id == 0u)
                {
                    net_addr_t relay =
                        (entry->iter_nexthop_addr.family == AF_INET || entry->iter_nexthop_addr.family == AF_INET6)
                            ? entry->iter_nexthop_addr
                            : entry->nexthop_addr;
                    uint32_t relay_oif = (entry->iter_out_ifindex != 0u) ? entry->iter_out_ifindex : entry->out_ifindex;
                    route_nhobj_set_relay(path->nexthop_id, &relay, relay_oif);
                }
                if (route_worker_post_calc_event(&head->key) != 0)
                {
                    route_work_handle_calc_event(&head->key);
                }
            }
        }
        route_recompute_iter_paths();
    }

    worker_send_inject_ack(msg, (ret >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL);
    dev_ipc_message_free(msg);
}

static void worker_handle_subscribe(dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_subscribe_req_t))
    {
        LOG_WARN("[route_worker] SUBSCRIBE payload 长度不足: %u", msg->payload_len);
        dev_ipc_message_free(msg);
        return;
    }

    const route_subscribe_req_t *req = (const route_subscribe_req_t *)msg->payload;
    uint32_t protocol = req->protocol;
    uint32_t vrf_id = req->vrf_id;
    uint16_t afi = req->afi;
    uint32_t flags = req->flags;

    for (GList *l = g_route_work_local->subscribers; l; l = l->next)
    {
        route_subscriber_t *sub = (route_subscriber_t *)l->data;
        if (sub->module_id == msg->src_module_id && sub->protocol == protocol && sub->vrf_id == vrf_id &&
            sub->afi == afi)
        {
            LOG_DEBUG("[route_worker] module 0x%08X 重复订阅，忽略", msg->src_module_id);
            if (flags & ROUTE_SUBSCRIBE_FLAG_FULL)
            {
                route_calc_pub_dump(msg->src_module_id, protocol, vrf_id, afi, msg->request_id);
            }
            else
            {
                route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc0(sizeof(route_msg_ack_t));
                ack->result = ERRCODE_SUCCESS;
                dev_ipc_message_t *resp =
                    dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id, msg->request_id,
                                           ack, sizeof(route_msg_ack_t), g_free);
                dev_ipc_send_response(route_local_ipc_ctx(), resp);
                dev_ipc_message_free(resp);
            }
            dev_ipc_message_free(msg);
            return;
        }
    }

    route_subscriber_t *sub = (route_subscriber_t *)g_malloc(sizeof(route_subscriber_t));
    sub->module_id = msg->src_module_id;
    sub->protocol = protocol;
    sub->vrf_id = vrf_id;
    sub->afi = afi;
    g_route_work_local->subscribers = g_list_append(g_route_work_local->subscribers, sub);

    LOG_INFO("[route_worker] module 0x%08X 订阅路由: protocol=%u vrf=%u afi=%u flags=0x%X", msg->src_module_id,
             protocol, vrf_id, afi, flags);

    if (flags & ROUTE_SUBSCRIBE_FLAG_FULL)
    {
        route_calc_pub_dump(msg->src_module_id, protocol, vrf_id, afi, msg->request_id);
    }
    else
    {
        route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc0(sizeof(route_msg_ack_t));
        ack->result = ERRCODE_SUCCESS;
        dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                         msg->request_id, ack, sizeof(route_msg_ack_t), g_free);
        dev_ipc_send_response(route_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }

    dev_ipc_message_free(msg);
}

static void worker_handle_unsubscribe(dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_subscribe_req_t))
    {
        dev_ipc_message_free(msg);
        return;
    }

    const route_subscribe_req_t *req = (const route_subscribe_req_t *)msg->payload;
    uint32_t protocol = req->protocol;
    uint32_t vrf_id = req->vrf_id;
    uint16_t afi = req->afi;

    GList *l = g_route_work_local->subscribers;
    while (l)
    {
        route_subscriber_t *sub = (route_subscriber_t *)l->data;
        GList *next = l->next;
        if (sub->module_id == msg->src_module_id && sub->protocol == protocol && sub->vrf_id == vrf_id &&
            sub->afi == afi)
        {
            g_route_work_local->subscribers = g_list_delete_link(g_route_work_local->subscribers, l);
            g_free(sub);
            LOG_INFO("[route_worker] module 0x%08X 取消订阅: protocol=%u vrf=%u afi=%u", msg->src_module_id, protocol,
                     vrf_id, afi);
            break;
        }
        l = next;
    }

    route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc0(sizeof(route_msg_ack_t));
    ack->result = ERRCODE_SUCCESS;
    dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                     msg->request_id, ack, sizeof(route_msg_ack_t), g_free);
    dev_ipc_send_response(route_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
    dev_ipc_message_free(msg);
}

// ============================================================================
// 命令分发
// ============================================================================

/**
 * @brief 处理单条 worker 命令（在 worker 线程调用）
 * @return 1 表示收到 SHUTDOWN 命令，0 表示正常处理
 */
static int worker_dispatch_cmd(route_worker_cmd_t *cmd)
{
    if (!cmd)
    {
        return 0;
    }

    int stop = 0;

    switch (cmd->type)
    {
        case ROUTE_WORKER_CMD_INJECT:
            worker_handle_inject(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_NH_REGISTER:
            route_relay_handle_nh_register(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_NH_UNREGISTER:
            route_relay_handle_nh_unregister(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_NHOBJ_ACQUIRE:
            worker_handle_nhobj_acquire(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_NHOBJ_RELEASE:
            worker_handle_nhobj_release(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_SUBSCRIBE:
            worker_handle_subscribe(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_UNSUBSCRIBE:
            worker_handle_unsubscribe(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_CLI_SHOW:
            /* show 命令和 continue 消息，在 worker 线程中访问内存 RIB */
            route_show_dispatch(cmd->msg);
            dev_ipc_message_free(cmd->msg);
            cmd->msg = NULL;
            break;

        case ROUTE_WORKER_CMD_APPLY:
        {
            /* 配置应用：按操作类型分发到对应 apply 函数 */
            route_apply_cmd_t *apply = cmd->apply;
            if (apply)
            {
                switch (apply->op)
                {
                    case ROUTE_APPLY_STATIC_ADD:
                    case ROUTE_APPLY_STATIC_DEL:
                    case ROUTE_APPLY_STATIC_DEL_PREFIX:
                        route_cfg_apply_static(apply);
                        break;
                    case ROUTE_APPLY_BATCH_ADD:
                    case ROUTE_APPLY_BATCH_DEL:
                        route_cfg_apply_batch(apply);
                        break;
                    default:
                        LOG_WARN("[route_worker] 未知 apply op: %d", (int)apply->op);
                        apply->rc = -1;
                        break;
                }
            }
            worker_cmd_complete(cmd, apply ? apply->rc : -1);
            /* waitable cmd 由 IPC 线程在 dispatch_apply 返回后自行销毁 */
            return 0;
        }

        case ROUTE_WORKER_CMD_IF_EVENT:
            /* IF 事件（UP/DOWN/ADDR_ADD/ADDR_DEL）：更新 IF 缓存，重算静态路由与 nexthop watch */
            LOG_DEBUG("[route_worker] 收到 IF 事件，触发 static/nexthop 重检查");
            if_api_cache_on_event(cmd->msg);
            route_static_on_if_change();
            route_recompute_iter_paths();
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            break;

        case ROUTE_WORKER_CMD_IF_DOWN:
            /* IF 模块下线（process stop/crash）：
             *   1) 清 IF 共享缓存，避免 route_nh_resolve 仍认为接口 up。
             *   2) route_recompute_iter_paths 重算所有已注册 nexthop watch，resolved
             *      变化时 route_relay_notify_state → NH_NOTIFY 通知 BGP 等 owner。
             *   3) route_static_on_if_change 撤销 interface-only 静态路由。
             * IF READY 后由 route_on_if_event_cb → if_api_subscribe_all 重建订阅。 */
            LOG_INFO("[route_worker] IF DOWN detected, flushing IF cache + recomputing nexthop watches");
            if_api_cache_cleanup();
            if_api_cache_init();
            route_recompute_iter_paths();
            route_static_on_if_change();
            break;

        case ROUTE_WORKER_CMD_FIB_ROUTE_RESULT:
            if (cmd->msg && cmd->msg->payload && cmd->msg->payload_len >= sizeof(fib_route_result_t))
            {
                const fib_route_result_t *result = (const fib_route_result_t *)cmd->msg->payload;
                if (result->op_msg_type == FIB_MSG_TYPE_ROUTE_UPSERT && result->result != ERRCODE_SUCCESS)
                {
                    route_calc_schedule_fib_retry(result->entry.vrf_id, result->entry.afi, &result->entry.prefix_addr,
                                                  result->entry.prefix_len);
                }
            }
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            break;

        case ROUTE_WORKER_CMD_VRF_EVENT:
        {
            /* SMOOTHSTART/EVENT 直接交给 lib：DOWN 路径已在前面拆掉非 public VRF 业务，
             * 这里只负责让 cache 跟随 REPLAY 流重建。 */
            uint32_t vrf_evt = 0;
            uint32_t vrf_evt_id = 0;
            if (cmd->msg && cmd->msg->payload && cmd->msg->payload_len >= offsetof(vrf_event_msg_t, rts))
            {
                const vrf_event_msg_t *evt = (const vrf_event_msg_t *)cmd->msg->payload;
                vrf_evt = evt->event;
                vrf_evt_id = evt->vrf_id;
            }

            if (vrf_evt == VRF_EVENT_SMOOTHSTART)
            {
                route_worker_purge_non_public_vrf_business();
            }
            else if (vrf_evt == VRF_EVENT_VRF_DEL)
            {
                worker_withdraw_loopback_routes(vrf_evt_id);
            }

            vrf_api_cache_on_event(cmd->msg);

            if (vrf_evt == VRF_EVENT_VRF_ADD)
            {
                worker_install_loopback_routes(vrf_evt_id);
            }

            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            worker_cmd_complete(cmd, ERRCODE_SUCCESS);
            return 0;
        }

        case ROUTE_WORKER_CMD_VRF_DOWN:
            /* VRF 模块 DOWN：先拆 RIB 中所有非 public VRF 的静态路由，再清 vrf_api cache。 */
            route_worker_purge_non_public_vrf_business();
            vrf_api_cache_clear();
            worker_cmd_complete(cmd, ERRCODE_SUCCESS);
            return 0;

        case ROUTE_WORKER_CMD_VRF_QUERY:
            worker_cmd_complete(cmd, worker_resolve_vrf_id_by_name(cmd->vrf_name, cmd->vrf_id_out));
            return 0;

        case ROUTE_WORKER_CMD_SHUTDOWN:
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            g_route_work_local->running = 0;
            stop = 1;
            break;

        default:
            LOG_WARN("[route_worker] 未知命令类型: %d", (int)cmd->type);
            if (cmd->msg)
            {
                dev_ipc_message_free(cmd->msg);
                cmd->msg = NULL;
            }
            break;
    }

    g_free(cmd);
    return stop;
}

/**
 * @brief 处理 cmd_eventfd 触发：排干所有待处理命令
 * @return 1 表示收到 SHUTDOWN，0 表示正常
 */
static int worker_drain_cmd_queue(void)
{
    uint64_t v;
    while (read(g_route_work_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
        /* 排干 eventfd 计数 */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("[route_worker] cmd eventfd read 失败");
    }

    route_worker_cmd_t *cmd = NULL;
    while ((cmd = (route_worker_cmd_t *)g_async_queue_try_pop(g_route_work_local->cmd_queue)) != NULL)
    {
        if (worker_dispatch_cmd(cmd))
        {
            return 1;
        }
    }
    return 0;
}

static void route_worker_drain_work_queue(route_work_local_t *wl)
{
    if (!wl || wl->work_eventfd < 0 || !wl->work_queue)
    {
        return;
    }

    uint64_t v;
    while (read(wl->work_eventfd, &v, sizeof(v)) > 0)
    {
        /* 排干 eventfd 计数 */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("[route_worker] work eventfd read 失败");
    }

    int processed = 0;
    route_worker_event_t *evt = NULL;
    while (processed < ROUTE_WORK_BATCH_SIZE &&
           (evt = (route_worker_event_t *)g_async_queue_try_pop(wl->work_queue)) != NULL)
    {
        switch (evt->type)
        {
            case ROUTE_WORK_EVENT_CALC:
                route_work_handle_calc_event(&evt->u.key);
                break;
            default:
                LOG_WARN("[route_worker] 未知工作事件类型: %d", (int)evt->type);
                break;
        }

        worker_event_destroy(evt);
        processed++;
    }

    if (g_async_queue_length(wl->work_queue) > 0)
    {
        worker_signal_work_event();
    }
}

// ============================================================================
// worker 线程主循环
// ============================================================================

static void *route_worker_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "route-worker");
    log_set_tag("route");
    vrf_api_cache_init();

    route_work_local_t *wl = g_route_work_local;
    struct epoll_event events[ROUTE_MAX_EPOLL_EVENTS];

    LOG_INFO("[route_worker] worker 线程启动");

    /* 公网 VRF（vrf_id=0）启动时即安装本地回环路由 */
    worker_install_loopback_routes(VRF_PUBLIC_VRF_ID);

    /* 恢复结束后做一次全量 nexthop 重算（在 route worker 线程内执行） */
    route_recompute_iter_paths();

    while (wl->running)
    {
        int n = epoll_wait(wl->epoll_fd, events, ROUTE_MAX_EPOLL_EVENTS, 1000);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("[route_worker] epoll_wait 失败");
            break;
        }

        for (int i = 0; i < n; i++)
        {
            if (events[i].data.ptr == (void *)&g_route_cmd_tag)
            {
                if (worker_drain_cmd_queue())
                {
                    goto out;
                }
                continue;
            }

            if (events[i].data.ptr == (void *)&g_route_work_tag)
            {
                route_worker_drain_work_queue(wl);
                continue;
            }

            LOG_WARN("[route_worker] 未知 epoll 事件 ptr=%p", events[i].data.ptr);
        }

        /* 定时处理 route_calc 延迟任务（例如 OS install 失败后的重试）。 */
        route_calc_on_periodic();
    }

out:
    LOG_INFO("[route_worker] worker 线程退出");
    vrf_api_cache_cleanup();
    return NULL;
}

// ============================================================================
// 生命周期
// ============================================================================

int route_worker_prepare(void)
{
    if (!g_route_work_local)
    {
        g_route_work_local = (route_work_local_t *)g_malloc0(sizeof(route_work_local_t));
        if (!g_route_work_local)
        {
            LOG_ERROR("[route_worker] route work local 分配失败");
            return -1;
        }

        g_route_work_local->epoll_fd = -1;
        g_route_work_local->cmd_eventfd = -1;
        g_route_work_local->work_eventfd = -1;

        g_route_work_local->rib = route_rib_create();
        if (!g_route_work_local->rib)
        {
            LOG_ERROR("[route_worker] RIB 创建失败");
            goto fail;
        }

        route_static_init();
        if_api_cache_init();
        route_calc_init();
        route_nhobj_init();
    }

    g_route_work_local->epoll_fd = -1;
    g_route_work_local->cmd_eventfd = -1;
    g_route_work_local->work_eventfd = -1;
    g_route_work_local->running = 1;

    g_route_work_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_route_work_local->epoll_fd < 0)
    {
        LOG_PERROR("[route_worker] epoll_create1 失败");
        goto fail;
    }

    g_route_work_local->cmd_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_route_work_local->cmd_eventfd < 0)
    {
        LOG_PERROR("[route_worker] eventfd 失败");
        goto fail;
    }

    g_route_work_local->work_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_route_work_local->work_eventfd < 0)
    {
        LOG_PERROR("[route_worker] work eventfd 失败");
        goto fail;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_route_cmd_tag;
    if (epoll_ctl(g_route_work_local->epoll_fd, EPOLL_CTL_ADD, g_route_work_local->cmd_eventfd, &ev) < 0)
    {
        LOG_PERROR("[route_worker] epoll_ctl ADD cmd_eventfd 失败");
        goto fail;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_route_work_tag;
    if (epoll_ctl(g_route_work_local->epoll_fd, EPOLL_CTL_ADD, g_route_work_local->work_eventfd, &ev) < 0)
    {
        LOG_PERROR("[route_worker] epoll_ctl ADD work_eventfd 失败");
        goto fail;
    }

    g_route_work_local->cmd_queue = g_async_queue_new();
    if (!g_route_work_local->cmd_queue)
    {
        LOG_ERROR("[route_worker] 命令队列创建失败");
        goto fail;
    }

    g_route_work_local->work_queue = g_async_queue_new();
    if (!g_route_work_local->work_queue)
    {
        LOG_ERROR("[route_worker] 工作事件队列创建失败");
        goto fail;
    }

    LOG_INFO("[route_worker] worker 资源初始化完成");
    return 0;

fail:
    route_worker_shutdown();
    return -1;
}

int route_worker_launch(void)
{
    if (!g_route_work_local)
    {
        return -1;
    }

    if (pthread_create(&g_route_work_local->thread, NULL, route_worker_thread_fn, NULL) != 0)
    {
        LOG_PERROR("[route_worker] pthread_create 失败");
        return -1;
    }

    LOG_INFO("[route_worker] worker 线程已启动");
    return 0;
}

int route_worker_post(route_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    route_worker_cmd_t *cmd = worker_cmd_create(type, msg, 0);
    if (!cmd)
    {
        return -1;
    }
    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return -1;
    }
    return 0;
}

int route_worker_post_show_cli(dev_ipc_message_t *msg)
{
    route_worker_cmd_t *cmd = worker_cmd_create(ROUTE_WORKER_CMD_CLI_SHOW, msg, 0);
    if (!cmd)
    {
        return -1;
    }
    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return -1;
    }
    return 0;
}

int route_worker_dispatch_apply(route_apply_cmd_t *apply)
{
    if (!apply)
    {
        return -1;
    }

    route_worker_cmd_t *cmd = worker_cmd_create(ROUTE_WORKER_CMD_APPLY, NULL, 1);
    if (!cmd)
    {
        return -1;
    }
    cmd->apply = apply;

    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return -1;
    }

    worker_cmd_wait(cmd);
    worker_cmd_destroy(cmd);
    return 0;
}

int route_worker_dispatch_vrf_event(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return -1;
    }

    route_worker_cmd_t *cmd = worker_cmd_create(ROUTE_WORKER_CMD_VRF_EVENT, msg, 1);
    if (!cmd)
    {
        return -1;
    }

    if (worker_cmd_enqueue(cmd) != 0)
    {
        cmd->msg = NULL;
        worker_cmd_destroy(cmd);
        return -1;
    }

    int rc = worker_cmd_wait(cmd);
    worker_cmd_destroy(cmd);
    return rc;
}

int route_worker_resolve_vrf_id_by_name(const char *vrf_name, uint32_t *vrf_id)
{
    if (!vrf_id || !g_route_work_local || !g_route_work_local->running || g_route_work_local->thread == 0)
    {
        return ERRCODE_FAIL;
    }

    route_worker_cmd_t *cmd = worker_cmd_create(ROUTE_WORKER_CMD_VRF_QUERY, NULL, 1);
    if (!cmd)
    {
        return ERRCODE_FAIL;
    }
    if (vrf_name)
    {
        g_strlcpy(cmd->vrf_name, vrf_name, sizeof(cmd->vrf_name));
    }
    cmd->vrf_id_out = vrf_id;

    if (worker_cmd_enqueue(cmd) != 0)
    {
        worker_cmd_destroy(cmd);
        return ERRCODE_FAIL;
    }

    int rc = worker_cmd_wait(cmd);
    worker_cmd_destroy(cmd);
    return rc;
}

void route_worker_shutdown(void)
{
    if (!g_route_work_local)
    {
        return;
    }

    if (g_route_work_local->running && g_route_work_local->thread != 0)
    {
        route_worker_cmd_t *cmd = (route_worker_cmd_t *)g_malloc0(sizeof(route_worker_cmd_t));
        if (cmd)
        {
            cmd->type = ROUTE_WORKER_CMD_SHUTDOWN;
            cmd->msg = NULL;
            g_async_queue_push(g_route_work_local->cmd_queue, cmd);
            worker_signal_cmd_event();
        }

        pthread_join(g_route_work_local->thread, NULL);
        g_route_work_local->thread = 0;
    }

    /* 排干剩余命令，对 waitable 命令通知失败 */
    if (g_route_work_local->cmd_queue)
    {
        route_worker_cmd_t *c = NULL;
        while ((c = (route_worker_cmd_t *)g_async_queue_try_pop(g_route_work_local->cmd_queue)) != NULL)
        {
            if (c->msg)
            {
                dev_ipc_message_free(c->msg);
            }
            if (c->waitable && !c->done)
            {
                worker_cmd_complete(c, ERRCODE_FAIL);
            }
            if (!c->waitable)
            {
                worker_cmd_destroy(c);
            }
        }
        g_async_queue_unref(g_route_work_local->cmd_queue);
        g_route_work_local->cmd_queue = NULL;
    }

    if (g_route_work_local->work_queue)
    {
        route_worker_event_t *evt = NULL;
        while ((evt = (route_worker_event_t *)g_async_queue_try_pop(g_route_work_local->work_queue)) != NULL)
        {
            worker_event_destroy(evt);
        }
        g_async_queue_unref(g_route_work_local->work_queue);
        g_route_work_local->work_queue = NULL;
    }

    if (g_route_work_local->epoll_fd >= 0)
    {
        close(g_route_work_local->epoll_fd);
        g_route_work_local->epoll_fd = -1;
    }

    if (g_route_work_local->cmd_eventfd >= 0)
    {
        close(g_route_work_local->cmd_eventfd);
        g_route_work_local->cmd_eventfd = -1;
    }

    if (g_route_work_local->work_eventfd >= 0)
    {
        close(g_route_work_local->work_eventfd);
        g_route_work_local->work_eventfd = -1;
    }

    /* 主动给依赖 ROUTE 的模块发"撤销"通知:
     *   1) NH-iter watcher (BGP/ISIS 等):resolved=0 NH_NOTIFY → 它们立即把对应 nexthop 标不可达
     *   2) ROUTE 订阅者:对每个 OS-installed best path 发 ROUTE_MSG_TYPE_UPDATE(is_withdraw=1)
     * 二者必须在 route_relay_cleanup / RIB destroy 之前调,IPC ctx 此时仍可用。 */
    route_relay_publish_unreachable_for_shutdown();
    route_pub_withdraw_all_for_shutdown();

    route_relay_cleanup();
    route_show_cleanup_state();
    if_api_cache_cleanup();
    route_static_cleanup();
    route_calc_cleanup();
    /* route_nhobj 须在 route_calc_cleanup 之后清理：calc cleanup 会走 calc_fib_withdraw 释放对象引用 */
    route_nhobj_cleanup();

    /* 释放业务数据 */
    if (g_route_work_local->rib)
    {
        route_rib_destroy(g_route_work_local->rib);
        g_route_work_local->rib = NULL;
    }
    g_list_free_full(g_route_work_local->subscribers, g_free);
    g_route_work_local->subscribers = NULL;
    g_list_free_full(g_route_work_local->batch_entries, g_free);
    g_route_work_local->batch_entries = NULL;

    g_free(g_route_work_local);
    g_route_work_local = NULL;

    LOG_INFO("[route_worker] worker 资源已释放");
}

int route_add_and_notify_nexthop_id(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                                    uint32_t protocol, const net_addr_t *source_addr, uint32_t nexthop_id,
                                    int32_t metric, int32_t preference, uint32_t out_ifindex, uint8_t nh_type)
{
    if (!g_route_work_local || !g_route_work_local->rib || !prefix_addr || !source_addr || nexthop_id == 0u)
    {
        return ERRCODE_FAIL;
    }

    /* 只带 nexthop_id：relay 已由对象维护（发布方在「添加下一跳」时写入），此处不 set_relay */
    int ret = route_rib_add_nexthop_id(g_route_work_local->rib, vrf_id, afi, prefix_addr, prefix_len, protocol,
                                       source_addr, nexthop_id, metric, preference, out_ifindex, nh_type, 0u, 0u, 0u);
    if (ret < 0)
    {
        return ret;
    }

    const route_head_t *head = route_rib_lookup_head(g_route_work_local->rib, vrf_id, afi, prefix_addr, prefix_len);
    if (!head)
    {
        return ret;
    }

    if (route_worker_post_calc_event(&head->key) != 0)
    {
        route_work_handle_calc_event(&head->key);
    }

    return ret;
}
