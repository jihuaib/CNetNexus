/**
 * @file   bgp_bmp_thread.c
 * @brief  BMP 独立线程实现：epoll 事件循环、命令队列、事件分发
 * @author jhb
 * @date   2026/04/04
 */
#include "bgp_bmp_thread.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "bgp_bmp.h"
#include "bgp_bmp_cfg_apply.h"
#include "bgp_bmp_cli.h"
#include "bgp_main.h"
#include "bgp_worker.h"
#include "cli.h"
#include "errcode.h"
#include "log.h"

/** BMP epoll 单次最大事件数 */
#define BMP_MAX_EPOLL_EVENTS 16

bgp_bmp_local_t *g_bgp_bmp_local = NULL;

/** epoll data.ptr sentinel：命令队列事件 */
static char bmp_cmd_tag;

// ============================================================================
// BMP 命令队列
// ============================================================================

/** BMP 线程命令类型 */
typedef enum bgp_bmp_cmd_type
{
    BMP_CMD_APPLY = 1,     /**< 同步配置应用 */
    BMP_CMD_SHUTDOWN = 2,  /**< 停止线程 */
    BMP_CMD_EVENT = 3,     /**< 异步 BGP 事件通知 */
    BMP_CMD_SHOW = 4,      /**< show 命令 */
    BMP_CMD_CLEAR_ALL = 5, /**< 同步清空所有 BMP 实例 */
} bgp_bmp_cmd_type_t;

/** BMP 事件类型 */
typedef enum bgp_bmp_event_type
{
    BMP_EVENT_PEER_UP = 1,
    BMP_EVENT_PEER_DOWN = 2,
    BMP_EVENT_ROUTE_MONITORING = 3,
    BMP_EVENT_INITIAL_PEER_UP = 4, /**< 初始 peer up（指定 inst_name） */
} bgp_bmp_event_type_t;

/** BMP 事件数据 */
typedef struct bgp_bmp_event
{
    bgp_bmp_event_type_t type;
    bgp_bmp_peer_info_t peer;
    char inst_name[BGP_BMP_INST_NAME_MAX]; /**< 仅 INITIAL_PEER_UP 使用 */
    union
    {
        struct
        {
            uint8_t reason;
        } peer_down;
        struct
        {
            uint8_t *pdu; /**< 堆分配的 PDU 副本 */
            uint16_t pdu_len;
        } route_mon;
    } u;
} bgp_bmp_event_t;

/** BMP 线程命令结构 */
typedef struct bgp_bmp_cmd
{
    bgp_bmp_cmd_type_t type;
    bgp_apply_cmd_t *apply; /**< BMP_CMD_APPLY（借用引用） */
    bgp_bmp_event_t *event; /**< BMP_CMD_EVENT（持有所有权） */
    dev_ipc_message_t *msg; /**< BMP_CMD_SHOW（持有所有权） */
    gboolean waitable;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    gboolean done;
    int rc;
} bgp_bmp_cmd_t;

static bgp_bmp_cmd_t *bmp_cmd_create(bgp_bmp_cmd_type_t type, gboolean waitable)
{
    bgp_bmp_cmd_t *cmd = g_malloc0(sizeof(*cmd));
    if (!cmd)
    {
        return NULL;
    }
    cmd->type = type;
    cmd->waitable = waitable;
    if (waitable)
    {
        pthread_mutex_init(&cmd->mutex, NULL);
        pthread_cond_init(&cmd->cond, NULL);
    }
    return cmd;
}

static void bmp_cmd_destroy(bgp_bmp_cmd_t *cmd)
{
    if (!cmd)
    {
        return;
    }
    if (cmd->event)
    {
        if (cmd->event->type == BMP_EVENT_ROUTE_MONITORING && cmd->event->u.route_mon.pdu)
        {
            g_free(cmd->event->u.route_mon.pdu);
        }
        g_free(cmd->event);
    }
    if (cmd->waitable)
    {
        pthread_mutex_destroy(&cmd->mutex);
        pthread_cond_destroy(&cmd->cond);
    }
    g_free(cmd);
}

static void bmp_cmd_complete(bgp_bmp_cmd_t *cmd, int rc)
{
    if (!cmd || !cmd->waitable)
    {
        return;
    }
    pthread_mutex_lock(&cmd->mutex);
    cmd->rc = rc;
    cmd->done = TRUE;
    pthread_cond_signal(&cmd->cond);
    pthread_mutex_unlock(&cmd->mutex);
}

static int bmp_cmd_wait(bgp_bmp_cmd_t *cmd)
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

static int bmp_cmd_enqueue(bgp_bmp_cmd_t *cmd)
{
    if (!cmd || !g_bgp_bmp_local || !g_bgp_bmp_local->cmd_queue || g_bgp_bmp_local->cmd_eventfd < 0)
    {
        return -1;
    }

    g_async_queue_push(g_bgp_bmp_local->cmd_queue, cmd);

    uint64_t one = 1;
    if (write(g_bgp_bmp_local->cmd_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("BMP: cmd eventfd write failed");
        }
    }

    return 0;
}

// ============================================================================
// Peer Info 快照
// ============================================================================

void bgp_bmp_fill_peer_info(bgp_session_t *sess, bgp_bmp_peer_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->neighbor_addr = sess->neighbor_addr;
    info->source_addr = sess->source_addr;
    info->remote_as = sess->remote_as;
    info->remote_id = sess->remote_id;
    info->remote_hold = sess->remote_hold;
    info->established_at_usec = sess->established_at_usec;

    if (sess->vrf)
    {
        info->local_hold_time = sess->vrf->hold_time;
        info->local_router_id = sess->vrf->router_id;
    }

    /* local_as 从 protocol 获取（BGP worker 线程安全） */
    if (g_bgp_work_local && g_bgp_work_local->protocol)
    {
        info->local_as = g_bgp_work_local->protocol->as_number;
    }
}

// ============================================================================
// 异步事件通知 API（BGP worker 线程调用）
// ============================================================================

void bgp_bmp_thread_notify_peer_up(bgp_session_t *sess)
{
    if (!g_bgp_bmp_local || !g_bgp_bmp_local->cmd_queue)
    {
        return;
    }

    bgp_bmp_cmd_t *cmd = bmp_cmd_create(BMP_CMD_EVENT, FALSE);
    if (!cmd)
    {
        return;
    }

    bgp_bmp_event_t *evt = g_malloc0(sizeof(*evt));
    if (!evt)
    {
        bmp_cmd_destroy(cmd);
        return;
    }

    evt->type = BMP_EVENT_PEER_UP;
    bgp_bmp_fill_peer_info(sess, &evt->peer);
    cmd->event = evt;

    if (bmp_cmd_enqueue(cmd) != 0)
    {
        bmp_cmd_destroy(cmd);
    }
}

void bgp_bmp_thread_notify_peer_down(bgp_session_t *sess, uint8_t reason)
{
    if (!g_bgp_bmp_local || !g_bgp_bmp_local->cmd_queue)
    {
        return;
    }

    bgp_bmp_cmd_t *cmd = bmp_cmd_create(BMP_CMD_EVENT, FALSE);
    if (!cmd)
    {
        return;
    }

    bgp_bmp_event_t *evt = g_malloc0(sizeof(*evt));
    if (!evt)
    {
        bmp_cmd_destroy(cmd);
        return;
    }

    evt->type = BMP_EVENT_PEER_DOWN;
    bgp_bmp_fill_peer_info(sess, &evt->peer);
    evt->u.peer_down.reason = reason;
    cmd->event = evt;

    if (bmp_cmd_enqueue(cmd) != 0)
    {
        bmp_cmd_destroy(cmd);
    }
}

void bgp_bmp_thread_notify_route_monitoring(bgp_session_t *sess, const uint8_t *bgp_pdu, uint16_t pdu_len)
{
    if (!g_bgp_bmp_local || !g_bgp_bmp_local->cmd_queue)
    {
        return;
    }
    if (!bgp_pdu || pdu_len == 0)
    {
        return;
    }

    bgp_bmp_cmd_t *cmd = bmp_cmd_create(BMP_CMD_EVENT, FALSE);
    if (!cmd)
    {
        return;
    }

    bgp_bmp_event_t *evt = g_malloc0(sizeof(*evt));
    if (!evt)
    {
        bmp_cmd_destroy(cmd);
        return;
    }

    evt->type = BMP_EVENT_ROUTE_MONITORING;
    bgp_bmp_fill_peer_info(sess, &evt->peer);

    /* 拷贝 PDU 到堆内存（原始缓冲区属于 BGP worker） */
    evt->u.route_mon.pdu = g_malloc(pdu_len);
    if (!evt->u.route_mon.pdu)
    {
        g_free(evt);
        bmp_cmd_destroy(cmd);
        return;
    }
    memcpy(evt->u.route_mon.pdu, bgp_pdu, pdu_len);
    evt->u.route_mon.pdu_len = pdu_len;
    cmd->event = evt;

    if (bmp_cmd_enqueue(cmd) != 0)
    {
        bmp_cmd_destroy(cmd);
    }
}

// ============================================================================
// 配置应用派发（BGP 主线程调用）
// ============================================================================

int bgp_bmp_dispatch_apply(bgp_apply_cmd_t *apply)
{
    if (!apply)
    {
        return -1;
    }

    bgp_bmp_cmd_t *cmd = bmp_cmd_create(BMP_CMD_APPLY, TRUE);
    if (!cmd)
    {
        return -1;
    }
    cmd->apply = apply;

    if (bmp_cmd_enqueue(cmd) != 0)
    {
        bmp_cmd_destroy(cmd);
        return -1;
    }

    int rc = bmp_cmd_wait(cmd);
    bmp_cmd_destroy(cmd);
    return rc;
}

int bgp_bmp_dispatch_clear_all(void)
{
    bgp_bmp_cmd_t *cmd = bmp_cmd_create(BMP_CMD_CLEAR_ALL, TRUE);
    if (!cmd)
    {
        return -1;
    }

    if (bmp_cmd_enqueue(cmd) != 0)
    {
        bmp_cmd_destroy(cmd);
        return -1;
    }

    int rc = bmp_cmd_wait(cmd);
    bmp_cmd_destroy(cmd);
    return (rc == ERRCODE_SUCCESS) ? 0 : -1;
}

// ============================================================================
// Show 命令转发（BGP 主线程调用）
// ============================================================================

int bgp_bmp_thread_post_show(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return -1;
    }

    bgp_bmp_cmd_t *cmd = bmp_cmd_create(BMP_CMD_SHOW, FALSE);
    if (!cmd)
    {
        return -1;
    }
    cmd->msg = msg;

    if (bmp_cmd_enqueue(cmd) != 0)
    {
        bmp_cmd_destroy(cmd);
        return -1;
    }

    return 0;
}

// ============================================================================
// Initial Peer Ups 请求（BMP 线程 → BGP worker）
// ============================================================================

/**
 * @brief 投递一条 initial peer up 事件到 BMP 线程
 *
 * 由 BGP worker 线程在处理 BMP_INITIAL_PEERS 命令时调用。
 */
void bgp_bmp_post_initial_peer_up(const bgp_bmp_peer_info_t *peer, const char *inst_name)
{
    if (!g_bgp_bmp_local || !g_bgp_bmp_local->cmd_queue)
    {
        return;
    }

    bgp_bmp_cmd_t *cmd = bmp_cmd_create(BMP_CMD_EVENT, FALSE);
    if (!cmd)
    {
        return;
    }

    bgp_bmp_event_t *evt = g_malloc0(sizeof(*evt));
    if (!evt)
    {
        bmp_cmd_destroy(cmd);
        return;
    }

    evt->type = BMP_EVENT_INITIAL_PEER_UP;
    evt->peer = *peer;
    g_strlcpy(evt->inst_name, inst_name, sizeof(evt->inst_name));
    cmd->event = evt;

    if (bmp_cmd_enqueue(cmd) != 0)
    {
        bmp_cmd_destroy(cmd);
    }
}

void bgp_bmp_request_initial_peers(const char *inst_name)
{
    bgp_worker_handle_bmp_initial_peers(inst_name);
}

// ============================================================================
// 事件处理（BMP 线程内）
// ============================================================================

/**
 * @brief 处理 peer_up 事件：遍历所有 BMP 实例，向匹配的发送 Peer Up
 */
static void bmp_handle_event_peer_up(const bgp_bmp_peer_info_t *peer)
{
    if (!g_bgp_bmp_local->bmp_instances)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&peer->neighbor_addr, addr_str, sizeof(addr_str));

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, g_bgp_bmp_local->bmp_instances);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;
        if (inst->conn_state == BGP_BMP_CONN_UP && bgp_bmp_should_monitor(inst, addr_str))
        {
            bgp_bmp_send_peer_up(inst, peer);
        }
    }
}

/**
 * @brief 处理 peer_down 事件
 */
static void bmp_handle_event_peer_down(const bgp_bmp_peer_info_t *peer, uint8_t reason)
{
    if (!g_bgp_bmp_local->bmp_instances)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&peer->neighbor_addr, addr_str, sizeof(addr_str));

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, g_bgp_bmp_local->bmp_instances);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;
        if (inst->conn_state == BGP_BMP_CONN_UP && bgp_bmp_should_monitor(inst, addr_str))
        {
            bgp_bmp_send_peer_down(inst, peer, reason);
        }
    }
}

/**
 * @brief 处理 route_monitoring 事件
 */
static void bmp_handle_event_route_monitoring(const bgp_bmp_peer_info_t *peer, const uint8_t *pdu, uint16_t pdu_len)
{
    if (!g_bgp_bmp_local->bmp_instances)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&peer->neighbor_addr, addr_str, sizeof(addr_str));

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, g_bgp_bmp_local->bmp_instances);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;
        if (inst->conn_state == BGP_BMP_CONN_UP && bgp_bmp_should_monitor(inst, addr_str))
        {
            bgp_bmp_send_route_monitoring(inst, peer, pdu, pdu_len);
        }
    }
}

/**
 * @brief 处理 initial peer up 事件（指定实例）
 */
static void bmp_handle_event_initial_peer_up(const bgp_bmp_peer_info_t *peer, const char *inst_name)
{
    if (!g_bgp_bmp_local->bmp_instances)
    {
        return;
    }

    bgp_bmp_instance_t *inst = g_hash_table_lookup(g_bgp_bmp_local->bmp_instances, inst_name);
    if (!inst || inst->conn_state != BGP_BMP_CONN_UP)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&peer->neighbor_addr, addr_str, sizeof(addr_str));

    if (bgp_bmp_should_monitor(inst, addr_str))
    {
        bgp_bmp_send_peer_up(inst, peer);
    }
}

/**
 * @brief 处理 BMP 事件
 */
static void bmp_dispatch_event(bgp_bmp_event_t *evt)
{
    switch (evt->type)
    {
        case BMP_EVENT_PEER_UP:
            bmp_handle_event_peer_up(&evt->peer);
            break;
        case BMP_EVENT_PEER_DOWN:
            bmp_handle_event_peer_down(&evt->peer, evt->u.peer_down.reason);
            break;
        case BMP_EVENT_ROUTE_MONITORING:
            bmp_handle_event_route_monitoring(&evt->peer, evt->u.route_mon.pdu, evt->u.route_mon.pdu_len);
            break;
        case BMP_EVENT_INITIAL_PEER_UP:
            bmp_handle_event_initial_peer_up(&evt->peer, evt->inst_name);
            break;
        default:
            break;
    }
}

// ============================================================================
// 配置应用分发（BMP 线程内）
// ============================================================================

static void bmp_dispatch_apply_cmd(bgp_apply_cmd_t *apply)
{
    if (!apply)
    {
        return;
    }
    apply->rc = BGP_APPLY_RC_FAIL;
    apply->errmsg[0] = '\0';

    switch (apply->group_id)
    {
        case BGP_CLI_GROUP_ID_BMP_INSTANCE:
            bgp_bmp_cfg_apply_instance(apply);
            break;
        case BGP_CLI_GROUP_ID_BMP_COLLECTOR:
            bgp_bmp_cfg_apply_collector(apply);
            break;
        case BGP_CLI_GROUP_ID_BMP_STATS_INTERVAL:
            bgp_bmp_cfg_apply_stats(apply);
            break;
        case BGP_CLI_GROUP_ID_BMP_RECONNECT:
            bgp_bmp_cfg_apply_reconnect(apply);
            break;
        case BGP_CLI_GROUP_ID_BMP_MONITOR:
            bgp_bmp_cfg_apply_monitor(apply);
            break;
        default:
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BMP Error: Unknown apply group_id %u.", apply->group_id);
            break;
    }
}

// ============================================================================
// Show 命令处理（BMP 线程内）
// ============================================================================

static const char *bmp_conn_state_str(bgp_bmp_conn_state_t st)
{
    switch (st)
    {
        case BGP_BMP_CONN_IDLE:
            return "Idle";
        case BGP_BMP_CONN_CONNECTING:
            return "Connecting";
        case BGP_BMP_CONN_UP:
            return "Up";
        case BGP_BMP_CONN_WAIT:
            return "Wait-Reconnect";
        default:
            return "Unknown";
    }
}

static void bmp_show_send_response(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_bgp_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

static void bmp_fmt_time_usec(gint64 usec, char *buf, size_t sz)
{
    if (usec <= 0)
    {
        g_strlcpy(buf, "-", sz);
        return;
    }
    time_t sec = (time_t)(usec / 1000000);
    struct tm tm_val;
    localtime_r(&sec, &tm_val);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tm_val);
}

static void bmp_show_instance_detail(GString *buf, const bgp_bmp_instance_t *inst)
{
    char collector_str[64] = "-";
    if (inst->collector_port > 0)
    {
        net_addr_to_str(&inst->collector_addr, collector_str, sizeof(collector_str));
    }

    g_string_append_printf(buf, "\r\nBMP Instance: %s\r\n", inst->name);
    g_string_append(buf, "------------------------------------------------------------\r\n");

    g_string_append(buf, "  Configuration:\r\n");
    if (inst->collector_port > 0)
    {
        g_string_append_printf(buf, "    Collector:          %s port %u\r\n", collector_str, inst->collector_port);
    }
    else
    {
        g_string_append(buf, "    Collector:          (not configured)\r\n");
    }
    g_string_append_printf(buf, "    Stats interval:     %u sec%s\r\n", inst->stats_interval,
                           inst->stats_interval == 0 ? " (disabled)" : "");
    g_string_append_printf(buf, "    Reconnect interval: %u sec\r\n", inst->reconnect_interval);
    if (inst->monitor_all)
    {
        g_string_append(buf, "    Monitor:            all neighbors\r\n");
    }
    else if (inst->monitor_peers && g_hash_table_size(inst->monitor_peers) > 0)
    {
        g_string_append(buf, "    Monitor:            specific neighbors\r\n");
        GHashTableIter piter;
        gpointer pkey, pval;
        g_hash_table_iter_init(&piter, inst->monitor_peers);
        while (g_hash_table_iter_next(&piter, &pkey, &pval))
        {
            g_string_append_printf(buf, "      - %s\r\n", (const char *)pkey);
        }
    }
    else
    {
        g_string_append(buf, "    Monitor:            none\r\n");
    }

    g_string_append(buf, "  Connection:\r\n");
    g_string_append_printf(buf, "    State:              %s\r\n", bmp_conn_state_str(inst->conn_state));

    if (inst->connected_at_usec > 0)
    {
        char time_buf[32];
        bmp_fmt_time_usec(inst->connected_at_usec, time_buf, sizeof(time_buf));
        g_string_append_printf(buf, "    Connected since:    %s\r\n", time_buf);
    }

    g_string_append(buf, "  Statistics:\r\n");
    g_string_append_printf(buf, "    Initiation sent:    %u\r\n", inst->initiation_sent);
    g_string_append_printf(buf, "    Peer Up sent:       %u\r\n", inst->peer_up_sent);
    g_string_append_printf(buf, "    Peer Down sent:     %u\r\n", inst->peer_down_sent);
    g_string_append_printf(buf, "    Route Monitor sent: %u\r\n", inst->route_monitor_sent);
    g_string_append_printf(buf, "    Stats Report sent:  %u\r\n", inst->stats_report_sent);
}

static void bmp_handle_show_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        dev_ipc_message_free(msg);
        return;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        bmp_show_send_response(msg, "BMP Error: Failed to parse command payload.\r\n");
        dev_ipc_message_free(msg);
        return;
    }

    char inst_name[BGP_BMP_INST_NAME_MAX] = {0};
    cli_tlv_entry_t entry;
    while (cli_tlv_next(&parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(inst_name, sizeof(inst_name), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    GHashTable *bmp_ht = g_bgp_bmp_local->bmp_instances;

    if (inst_name[0] != '\0')
    {
        if (!bmp_ht)
        {
            bmp_show_send_response(msg, "BMP Error: No BMP instances configured.\r\n");
            cli_tlv_cleanup(&parser);
            dev_ipc_message_free(msg);
            return;
        }
        bgp_bmp_instance_t *inst = g_hash_table_lookup(bmp_ht, inst_name);
        if (!inst)
        {
            bmp_show_send_response(msg, "BMP Error: Instance not found.\r\n");
            cli_tlv_cleanup(&parser);
            dev_ipc_message_free(msg);
            return;
        }

        GString *resp_buf = g_string_new("");
        bmp_show_instance_detail(resp_buf, inst);
        bmp_show_send_response(msg, resp_buf->str);
        g_string_free(resp_buf, TRUE);
        cli_tlv_cleanup(&parser);
        dev_ipc_message_free(msg);
        return;
    }

    /* 无实例名 → 显示所有实例摘要 */
    GString *resp_buf = g_string_new("");
    g_string_append(resp_buf, "\r\nBMP Instances\r\n");
    g_string_append(resp_buf, "============================================================\r\n");

    if (!bmp_ht || g_hash_table_size(bmp_ht) == 0)
    {
        g_string_append(resp_buf, "  (no BMP instances configured)\r\n");
        bmp_show_send_response(msg, resp_buf->str);
        g_string_free(resp_buf, TRUE);
        cli_tlv_cleanup(&parser);
        dev_ipc_message_free(msg);
        return;
    }

    g_string_append_printf(resp_buf, "  %-20s%-18s%-8s%-14s%s\r\n", "Instance", "Collector", "Port", "State",
                           "PeerUp/Down");
    g_string_append_printf(resp_buf, "  %-20s%-18s%-8s%-14s%s\r\n", "-------------------", "-----------------",
                           "------", "------------", "-----------");

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, bmp_ht);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;

        char collector_str[64] = "-";
        char port_str[8] = "-";
        if (inst->collector_port > 0)
        {
            net_addr_to_str(&inst->collector_addr, collector_str, sizeof(collector_str));
            snprintf(port_str, sizeof(port_str), "%u", inst->collector_port);
        }

        char updown[32];
        snprintf(updown, sizeof(updown), "%u/%u", inst->peer_up_sent, inst->peer_down_sent);

        g_string_append_printf(resp_buf, "  %-20s%-18s%-8s%-14s%s\r\n", inst->name, collector_str, port_str,
                               bmp_conn_state_str(inst->conn_state), updown);
    }

    bmp_show_send_response(msg, resp_buf->str);
    g_string_free(resp_buf, TRUE);
    cli_tlv_cleanup(&parser);
    dev_ipc_message_free(msg);
}

// ============================================================================
// 命令队列处理（BMP 线程内）
// ============================================================================

static void bmp_cmd_drain_queue(void)
{
    if (!g_bgp_bmp_local->cmd_queue)
    {
        return;
    }

    bgp_bmp_cmd_t *cmd = NULL;
    while ((cmd = (bgp_bmp_cmd_t *)g_async_queue_try_pop(g_bgp_bmp_local->cmd_queue)) != NULL)
    {
        if (cmd->msg)
        {
            dev_ipc_message_free(cmd->msg);
            cmd->msg = NULL;
        }
        if (cmd->type == BMP_CMD_APPLY && cmd->apply)
        {
            cmd->apply->rc = BGP_APPLY_RC_FAIL;
            snprintf(cmd->apply->errmsg, sizeof(cmd->apply->errmsg), "BMP thread shutdown");
        }
        if (cmd->waitable && !cmd->done)
        {
            bmp_cmd_complete(cmd, ERRCODE_FAIL);
        }
        if (!cmd->waitable)
        {
            bmp_cmd_destroy(cmd);
        }
    }
}

static gboolean bmp_process_cmd_event(void)
{
    if (g_bgp_bmp_local->cmd_eventfd < 0 || !g_bgp_bmp_local->cmd_queue)
    {
        return FALSE;
    }

    uint64_t v;
    while (read(g_bgp_bmp_local->cmd_eventfd, &v, sizeof(v)) > 0)
    {
        /* drain eventfd */
    }

    gboolean stop = FALSE;
    bgp_bmp_cmd_t *cmd = NULL;
    while ((cmd = (bgp_bmp_cmd_t *)g_async_queue_try_pop(g_bgp_bmp_local->cmd_queue)) != NULL)
    {
        switch (cmd->type)
        {
            case BMP_CMD_APPLY:
                bmp_dispatch_apply_cmd(cmd->apply);
                break;

            case BMP_CMD_SHUTDOWN:
                g_bgp_bmp_local->running = 0;
                stop = TRUE;
                break;

            case BMP_CMD_EVENT:
                if (cmd->event)
                {
                    bmp_dispatch_event(cmd->event);
                }
                break;

            case BMP_CMD_SHOW:
                bmp_handle_show_msg(cmd->msg);
                cmd->msg = NULL; /* 所有权已转移 */
                break;

            case BMP_CMD_CLEAR_ALL:
                if (g_bgp_bmp_local->bmp_instances)
                {
                    GHashTableIter iter;
                    gpointer key = NULL;
                    gpointer val = NULL;
                    g_hash_table_iter_init(&iter, g_bgp_bmp_local->bmp_instances);
                    while (g_hash_table_iter_next(&iter, &key, &val))
                    {
                        (void)key;
                        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;
                        bgp_bmp_instance_destroy(inst, g_bgp_bmp_local->epoll_fd);
                    }
                    g_hash_table_remove_all(g_bgp_bmp_local->bmp_instances);
                }
                break;

            default:
                break;
        }

        if (cmd->waitable)
        {
            bmp_cmd_complete(cmd, ERRCODE_SUCCESS);
        }
        else
        {
            bmp_cmd_destroy(cmd);
        }

        if (stop)
        {
            break;
        }
    }

    return stop;
}

// ============================================================================
// 清理
// ============================================================================

static void bmp_runtime_cleanup(void)
{
    if (!g_bgp_bmp_local)
    {
        return;
    }

    /* 清理所有 BMP 实例 */
    if (g_bgp_bmp_local->bmp_instances)
    {
        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init(&iter, g_bgp_bmp_local->bmp_instances);
        while (g_hash_table_iter_next(&iter, &key, &val))
        {
            bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;
            bgp_bmp_instance_destroy(inst, g_bgp_bmp_local->epoll_fd);
        }
        g_hash_table_destroy(g_bgp_bmp_local->bmp_instances);
        g_bgp_bmp_local->bmp_instances = NULL;
    }

    bmp_cmd_drain_queue();
}

// ============================================================================
// BMP 线程主函数
// ============================================================================

static void *bgp_bmp_thread_fn(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "bgp-bmp");
    log_set_tag("bmp");

    struct epoll_event events[BMP_MAX_EPOLL_EVENTS];

    while (g_bgp_bmp_local->running)
    {
        int nfds = epoll_wait(g_bgp_bmp_local->epoll_fd, events, BMP_MAX_EPOLL_EVENTS, 1000);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("BMP: epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            uintptr_t raw = (uintptr_t)events[i].data.ptr;

            /* 命令队列事件 */
            if (events[i].data.ptr == (void *)&bmp_cmd_tag)
            {
                if (bmp_process_cmd_event())
                {
                    break;
                }
                continue;
            }

            /* BMP timer/conn 事件（sentinel bit0=1） */
            if (raw & 1UL)
            {
                bgp_bmp_timer_sentinel_t *sentinel = (bgp_bmp_timer_sentinel_t *)(raw & ~1UL);
                switch (sentinel->type)
                {
                    case BMP_TIMER_RECONNECT:
                    {
                        bgp_bmp_instance_t *bmp =
                            (bgp_bmp_instance_t *)((char *)sentinel - offsetof(bgp_bmp_instance_t, reconnect_sentinel));
                        bgp_bmp_handle_reconnect(bmp, g_bgp_bmp_local->epoll_fd);
                        break;
                    }
                    case BMP_TIMER_STATS:
                    {
                        bgp_bmp_instance_t *bmp =
                            (bgp_bmp_instance_t *)((char *)sentinel - offsetof(bgp_bmp_instance_t, stats_sentinel));
                        bgp_bmp_handle_stats_timer(bmp);
                        break;
                    }
                    case BMP_TIMER_CONN:
                    {
                        bgp_bmp_instance_t *bmp =
                            (bgp_bmp_instance_t *)((char *)sentinel - offsetof(bgp_bmp_instance_t, conn_sentinel));
                        if (events[i].events & EPOLLOUT)
                        {
                            bgp_bmp_handle_connect_result(bmp, g_bgp_bmp_local->epoll_fd);
                        }
                        else
                        {
                            bgp_bmp_handle_read(bmp, g_bgp_bmp_local->epoll_fd);
                        }
                        break;
                    }
                    default:
                        break;
                }
                continue;
            }
        }
    }

    bmp_runtime_cleanup();
    return NULL;
}

// ============================================================================
// 生命周期 API
// ============================================================================

int bgp_bmp_thread_prepare(void)
{
    if (!g_bgp_bmp_local)
    {
        g_bgp_bmp_local = g_malloc0(sizeof(*g_bgp_bmp_local));
        if (!g_bgp_bmp_local)
        {
            LOG_ERROR("BMP: Failed to allocate local context");
            return ERRCODE_FAIL;
        }
        g_bgp_bmp_local->epoll_fd = -1;
        g_bgp_bmp_local->cmd_eventfd = -1;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("BMP: Failed to create epoll");
        return ERRCODE_FAIL;
    }
    g_bgp_bmp_local->epoll_fd = epoll_fd;

    /* 命令队列 */
    g_bgp_bmp_local->cmd_queue = g_async_queue_new();
    if (!g_bgp_bmp_local->cmd_queue)
    {
        LOG_ERROR("BMP: Failed to create cmd queue");
        close(epoll_fd);
        g_bgp_bmp_local->epoll_fd = -1;
        return ERRCODE_FAIL;
    }

    g_bgp_bmp_local->cmd_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_bgp_bmp_local->cmd_eventfd < 0)
    {
        LOG_PERROR("BMP: Failed to create cmd eventfd");
        g_async_queue_unref(g_bgp_bmp_local->cmd_queue);
        g_bgp_bmp_local->cmd_queue = NULL;
        close(epoll_fd);
        g_bgp_bmp_local->epoll_fd = -1;
        return ERRCODE_FAIL;
    }

    /* 注册 cmd_eventfd 到 epoll */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &bmp_cmd_tag;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_bgp_bmp_local->cmd_eventfd, &ev) < 0)
    {
        LOG_PERROR("BMP: epoll_ctl ADD cmd eventfd failed");
        close(g_bgp_bmp_local->cmd_eventfd);
        g_bgp_bmp_local->cmd_eventfd = -1;
        g_async_queue_unref(g_bgp_bmp_local->cmd_queue);
        g_bgp_bmp_local->cmd_queue = NULL;
        close(epoll_fd);
        g_bgp_bmp_local->epoll_fd = -1;
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int bgp_bmp_thread_launch(void)
{
    g_bgp_bmp_local->running = 1;
    if (pthread_create(&g_bgp_bmp_local->thread, NULL, bgp_bmp_thread_fn, NULL) != 0)
    {
        LOG_PERROR("BMP: Failed to create thread");
        g_bgp_bmp_local->running = 0;
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

void bgp_bmp_thread_shutdown(void)
{
    if (!g_bgp_bmp_local)
    {
        return;
    }

    bgp_bmp_cmd_t *shutdown_cmd = NULL;

    if (g_bgp_bmp_local->thread)
    {
        shutdown_cmd = bmp_cmd_create(BMP_CMD_SHUTDOWN, TRUE);
        if (shutdown_cmd && bmp_cmd_enqueue(shutdown_cmd) == 0)
        {
            (void)bmp_cmd_wait(shutdown_cmd);
        }
        else
        {
            g_bgp_bmp_local->running = 0;
        }

        pthread_join(g_bgp_bmp_local->thread, NULL);
        g_bgp_bmp_local->thread = 0;
    }
    else
    {
        bmp_runtime_cleanup();
    }

    if (shutdown_cmd)
    {
        bmp_cmd_destroy(shutdown_cmd);
    }

    g_bgp_bmp_local->running = 0;

    /* 清理命令通道 */
    if (g_bgp_bmp_local->cmd_eventfd >= 0)
    {
        close(g_bgp_bmp_local->cmd_eventfd);
        g_bgp_bmp_local->cmd_eventfd = -1;
    }
    if (g_bgp_bmp_local->cmd_queue)
    {
        g_async_queue_unref(g_bgp_bmp_local->cmd_queue);
        g_bgp_bmp_local->cmd_queue = NULL;
    }
    if (g_bgp_bmp_local->epoll_fd >= 0)
    {
        close(g_bgp_bmp_local->epoll_fd);
        g_bgp_bmp_local->epoll_fd = -1;
    }

    g_free(g_bgp_bmp_local);
    g_bgp_bmp_local = NULL;
}
