/**
 * @file   sbmp_cli.c
 * @brief  SBMP 模块 CLI 命令处理实现
 * @author jhb
 * @date   2026/03/08
 */
#include "sbmp_cli.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "sbmp_db.h"
#include "sbmp_main.h"
#include "sbmp_rib.h"

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void sbmp_send_cli_response_typed(dev_ipc_message_t *msg, uint32_t msg_type, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(msg_type, DEV_MODULE_ID_SBMP, msg->src_module_id, msg->request_id,
                                                     resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(sbmp_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static void sbmp_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    sbmp_send_cli_response_typed(msg, CLI_MSG_TYPE_RESP, text);
}

int sbmp_cli_send_chunked_response(dev_ipc_message_t *msg, GString *full_text)
{
    return cli_chunk_stream_start(&g_sbmp_local->show_stream, sbmp_local_ipc_ctx(), DEV_MODULE_ID_SBMP, msg, full_text);
}

// ============================================================================
// 文本格式化辅助
// ============================================================================

static void fmt_time_usec(gint64 usec, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }

    if (usec <= 0)
    {
        snprintf(buf, sz, "-");
        return;
    }

    time_t sec = (time_t)(usec / 1000000);
    struct tm tmv;
    if (!localtime_r(&sec, &tmv))
    {
        snprintf(buf, sz, "-");
        return;
    }

    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tmv);
}

static const char *bgp_origin_str(bgp_origin_t origin)
{
    switch (origin)
    {
        case BGP_ORIGIN_IGP:
            return "IGP";
        case BGP_ORIGIN_EGP:
            return "EGP";
        case BGP_ORIGIN_INCOMPLETE:
            return "INCOMPLETE";
        default:
            return "UNKNOWN";
    }
}

static void bgp_nexthop_to_str(const bgp_nexthop_t *nexthop, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }

    char global[64] = "-";
    if (nexthop && nexthop->global.family != 0)
    {
        net_addr_to_str(&nexthop->global, global, sizeof(global));
    }

    if (nexthop && nexthop->has_link_local && nexthop->link_local.family != 0)
    {
        char ll[64];
        net_addr_to_str(&nexthop->link_local, ll, sizeof(ll));
        snprintf(buf, sz, "%s (ll:%s)", global, ll);
        return;
    }

    snprintf(buf, sz, "%s", global);
}

static const char *sbmp_policy_str(sbmp_route_policy_t policy)
{
    if (policy == SBMP_ROUTE_POLICY_POST)
    {
        return "post";
    }
    if (policy == SBMP_ROUTE_POLICY_LOC_RIB)
    {
        return "loc-rib";
    }
    return "pre";
}

static const sbmp_rib_t *sbmp_client_rib(const sbmp_client_t *client, uint16_t afi, uint8_t safi,
                                         sbmp_route_policy_t policy)
{
    if (!client || safi != BGP_SAFI_UNICAST)
    {
        return NULL;
    }

    if (afi == BGP_AFI_IPV4)
    {
        if (policy == SBMP_ROUTE_POLICY_POST)
        {
            return client->rib_v4u_post;
        }
        if (policy == SBMP_ROUTE_POLICY_LOC_RIB)
        {
            return client->rib_v4u_loc_rib;
        }
        return client->rib_v4u_pre;
    }
    if (afi == BGP_AFI_IPV6)
    {
        if (policy == SBMP_ROUTE_POLICY_POST)
        {
            return client->rib_v6u_post;
        }
        if (policy == SBMP_ROUTE_POLICY_LOC_RIB)
        {
            return client->rib_v6u_loc_rib;
        }
        return client->rib_v6u_pre;
    }
    return NULL;
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理 "bmp-server" / "no bmp-server" 命令
 *
 * group_id=1：仅控制视图切换，no 命令清除 server port 配置
 */
static int handle_bmp_server(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;

    /* CFG 已经卡 READY 才派发；READY 时业务已从 DB 恢复完，不再业务侧拦截 */

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }

    if (!is_no)
    {
        sbmp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (sbmp_db_del_server_port() < 0)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Database cleanup failed.\r\n");
        return ERRCODE_FAIL;
    }
    sbmp_listen_stop();
    g_sbmp_local->server_port = 0;

    sbmp_send_cli_response_typed(msg, CLI_MSG_TYPE_RESP_EXITING, "SBMP: configuration cleared, process exiting.\r\n");

    /* 配置清空后让进程自退出：DEV 的 SIGCHLD handler 会标记 REGISTERED + 推送 DOWN；
     * 下次用户配 bmp-server 时 CFG 的 wait_module_ready(SBMP) 会让 DEV 重新 fork。
     * 用 raise(SIGTERM) 触发 sbmp_proc.c 的 shutdown_handler，走优雅退出流程。 */
    /* kill(getpid(), SIGTERM) 而非 raise(SIGTERM)：raise 只送到调用线程，
     * 主线程的 sigsuspend 不会被唤醒；kill 是进程级信号，会派给可处理它的线程。 */
    kill(getpid(), SIGTERM);
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 "server port <port>" / "no server port" 命令
 *
 * group_id=2, cfg_id: 2=port-number
 */
static int handle_server_port(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t port = 0;

    /* CFG 已经卡 READY 才派发；READY 时业务已从 DB 恢复完，不再业务侧拦截 */

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 2)
        {
            port = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (is_no)
    {
        gboolean had_server_port = g_sbmp_local->server_port != 0;
        /*
         * 即使运行态已经为空也要执行幂等 DB 删除；只有 revive 表确认
         * 清空后才能发送 RESP_EXITING，避免残留配置在下次启动时复活。
         */
        if (sbmp_db_del_server_port() < 0)
        {
            sbmp_send_cli_response(msg, "SBMP Error: Database cleanup failed.\r\n");
            return ERRCODE_FAIL;
        }
        if (had_server_port)
        {
            sbmp_listen_stop();
        }
        g_sbmp_local->server_port = 0;
        sbmp_send_cli_response_typed(msg, CLI_MSG_TYPE_RESP_EXITING,
                                     had_server_port ? "SBMP: server port cleared, process exiting.\r\n"
                                                     : "SBMP: no config to remove, process exiting.\r\n");
        /* 与 no bmp-server 路径一致：清空配置后自退出，让 DEV 回到 on-demand 待命状态 */
        /* kill(getpid(), SIGTERM) 而非 raise(SIGTERM)：raise 只送到调用线程，
         * 主线程的 sigsuspend 不会被唤醒；kill 是进程级信号，会派给可处理它的线程。 */
        kill(getpid(), SIGTERM);
        return ERRCODE_SUCCESS;
    }

    if (port == 0 || port > 65535)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Invalid port number (1-65535).\r\n");
        return ERRCODE_FAIL;
    }

    if (g_sbmp_local->server_port == (uint16_t)port)
    {
        sbmp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (g_sbmp_local->server_port != 0)
    {
        sbmp_listen_stop();
    }

    sbmp_listen_start((uint16_t)port);
    if (g_sbmp_local->listen_fd < 0 || g_sbmp_local->server_port != (uint16_t)port)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Failed to start BMP server listener.\r\n");
        return ERRCODE_FAIL;
    }

    if (sbmp_db_set_server_port((uint16_t)port) != 0)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "SBMP: BMP server listening on port %u.\r\n", port);
    sbmp_send_cli_response(msg, resp);
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 "show bmp-server" 命令
 */
static int handle_show_bmp_server(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    uint32_t client_total = 0;
    uint32_t client_connected = 0;
    uint32_t peer_total = 0;
    uint32_t routes_pre_v4 = 0;
    uint32_t routes_pre_v6 = 0;
    uint32_t routes_post_v4 = 0;
    uint32_t routes_post_v6 = 0;
    uint32_t routes_loc_rib_v4 = 0;
    uint32_t routes_loc_rib_v6 = 0;

    sbmp_runtime_lock();
    if (g_sbmp_local->client_hash)
    {
        client_total = (uint32_t)g_hash_table_size(g_sbmp_local->client_hash);

        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init(&iter, g_sbmp_local->client_hash);
        while (g_hash_table_iter_next(&iter, &key, &val))
        {
            (void)key;
            sbmp_client_t *c = (sbmp_client_t *)val;
            if (c->connected)
            {
                client_connected++;
            }
            peer_total += (uint32_t)g_hash_table_size(c->peer_hash);
            routes_pre_v4 += sbmp_rib_route_count(c->rib_v4u_pre);
            routes_pre_v6 += sbmp_rib_route_count(c->rib_v6u_pre);
            routes_post_v4 += sbmp_rib_route_count(c->rib_v4u_post);
            routes_post_v6 += sbmp_rib_route_count(c->rib_v6u_post);
            routes_loc_rib_v4 += sbmp_rib_route_count(c->rib_v4u_loc_rib);
            routes_loc_rib_v6 += sbmp_rib_route_count(c->rib_v6u_loc_rib);
        }
    }
    sbmp_runtime_unlock();

    g_string_append(buf, "BMP Server Status\r\n");
    g_string_append(buf, "-----------------\r\n");

    if (g_sbmp_local->server_port == 0)
    {
        g_string_append(buf, "  State            : not configured\r\n");
    }
    else
    {
        const char *state = (g_sbmp_local->listen_fd >= 0) ? "running" : "stopped";
        g_string_append_printf(buf, "  Port             : %u\r\n", g_sbmp_local->server_port);
        g_string_append_printf(buf, "  State            : %s\r\n", state);
    }

    g_string_append_printf(buf, "  Clients(total/up): %u/%u\r\n", client_total, client_connected);
    g_string_append_printf(buf, "  Peers(total)     : %u\r\n", peer_total);
    g_string_append_printf(buf, "  Routes pre(v4/v6): %u/%u\r\n", routes_pre_v4, routes_pre_v6);
    g_string_append_printf(buf, "  Routes post(v4/v6): %u/%u\r\n", routes_post_v4, routes_post_v6);
    g_string_append_printf(buf, "  Routes loc-rib(v4/v6): %u/%u\r\n", routes_loc_rib_v4, routes_loc_rib_v6);

    return sbmp_cli_send_chunked_response(msg, buf);
}

/**
 * @brief 处理 "show bmp-server client [<client-id>]"
 *
 * group_id=4, cfg_id: 2=client-id
 */
static int handle_show_bmp_client(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char client_id[SBMP_CLIENT_ID_MAX] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (entry.cfg_id == 2)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(client_id, sizeof(client_id), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    sbmp_runtime_lock();

    if (client_id[0] != '\0')
    {
        sbmp_client_t *c = sbmp_runtime_find_client_locked(client_id);
        if (!c)
        {
            sbmp_runtime_unlock();
            g_string_free(buf, TRUE);
            sbmp_send_cli_response(msg, "SBMP Error: BMP client not found.\r\n");
            return ERRCODE_FAIL;
        }

        char t_conn[32], t_active[32];
        fmt_time_usec(c->connected_at_usec, t_conn, sizeof(t_conn));
        fmt_time_usec(c->last_active_usec, t_active, sizeof(t_active));

        g_string_append_printf(buf, "BMP Client: %s\r\n", c->client_id);
        g_string_append(buf, "----------------------------------------\r\n");
        g_string_append_printf(buf, "  Source          : %s:%u\r\n", c->source_ip, c->source_port);
        g_string_append_printf(buf, "  Connected       : %s\r\n", c->connected ? "yes" : "no");
        g_string_append_printf(buf, "  Connected-At    : %s\r\n", t_conn);
        g_string_append_printf(buf, "  Last-Active     : %s\r\n", t_active);
        g_string_append_printf(buf, "  Peers           : %u\r\n", (uint32_t)g_hash_table_size(c->peer_hash));
        g_string_append_printf(buf, "  RIB pre  IPv4   : heads=%u routes=%u\r\n", sbmp_rib_head_count(c->rib_v4u_pre),
                               sbmp_rib_route_count(c->rib_v4u_pre));
        g_string_append_printf(buf, "  RIB post IPv4   : heads=%u routes=%u\r\n", sbmp_rib_head_count(c->rib_v4u_post),
                               sbmp_rib_route_count(c->rib_v4u_post));
        g_string_append_printf(buf, "  RIB loc-rib IPv4: heads=%u routes=%u\r\n",
                               sbmp_rib_head_count(c->rib_v4u_loc_rib), sbmp_rib_route_count(c->rib_v4u_loc_rib));
        g_string_append_printf(buf, "  RIB pre  IPv6   : heads=%u routes=%u\r\n", sbmp_rib_head_count(c->rib_v6u_pre),
                               sbmp_rib_route_count(c->rib_v6u_pre));
        g_string_append_printf(buf, "  RIB post IPv6   : heads=%u routes=%u\r\n", sbmp_rib_head_count(c->rib_v6u_post),
                               sbmp_rib_route_count(c->rib_v6u_post));
        g_string_append_printf(buf, "  RIB loc-rib IPv6: heads=%u routes=%u\r\n",
                               sbmp_rib_head_count(c->rib_v6u_loc_rib), sbmp_rib_route_count(c->rib_v6u_loc_rib));
        g_string_append_printf(buf, "  BMP Msg(total/err): %llu/%llu\r\n", (unsigned long long)c->msg_total,
                               (unsigned long long)c->msg_parse_err);
        g_string_append_printf(buf, "  Msg RM(total/pre/post/loc): %llu/%llu/%llu/%llu\r\n",
                               (unsigned long long)c->route_monitor_msgs, (unsigned long long)c->route_monitor_pre_msgs,
                               (unsigned long long)c->route_monitor_post_msgs,
                               (unsigned long long)c->route_monitor_loc_rib_msgs);
        g_string_append_printf(buf, "  Msg UPDATE(pre/post/loc): %llu/%llu/%llu\r\n",
                               (unsigned long long)c->update_pre_msgs, (unsigned long long)c->update_post_msgs,
                               (unsigned long long)c->update_loc_rib_msgs);
        g_string_append_printf(buf, "  Msg UP/DOWN/STAT: %llu/%llu/%llu\r\n", (unsigned long long)c->peer_up_msgs,
                               (unsigned long long)c->peer_down_msgs, (unsigned long long)c->stats_report_msgs);
    }
    else
    {
        g_string_append(buf, "BMP Clients\r\n");
        g_string_append(buf, "-----------\r\n");
        g_string_append_printf(buf, "  %-18s %-10s %-6s %-5s %-14s %-14s\r\n", "Client-ID", "State", "Peers", "FD",
                               "V4(p/q/l)", "V6(p/q/l)");
        g_string_append_printf(buf, "  %-18s %-10s %-6s %-5s %-14s %-14s\r\n", "------------------", "----------",
                               "------", "-----", "--------------", "--------------");

        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init(&iter, g_sbmp_local->client_hash);
        while (g_hash_table_iter_next(&iter, &key, &val))
        {
            (void)key;
            sbmp_client_t *c = (sbmp_client_t *)val;
            g_string_append_printf(buf, "  %-18s %-10s %-6u %-5d %3u/%-3u/%-3u %3u/%-3u/%-3u\r\n", c->client_id,
                                   c->connected ? "connected" : "down", (uint32_t)g_hash_table_size(c->peer_hash),
                                   c->fd, sbmp_rib_route_count(c->rib_v4u_pre), sbmp_rib_route_count(c->rib_v4u_post),
                                   sbmp_rib_route_count(c->rib_v4u_loc_rib), sbmp_rib_route_count(c->rib_v6u_pre),
                                   sbmp_rib_route_count(c->rib_v6u_post), sbmp_rib_route_count(c->rib_v6u_loc_rib));
        }
    }

    sbmp_runtime_unlock();

    return sbmp_cli_send_chunked_response(msg, buf);
}

static void append_peer_rows(GString *buf, const sbmp_client_t *client, const char *peer_filter, uint32_t *listed)
{
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, client->peer_hash);

    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        (void)key;
        const sbmp_peer_t *p = (const sbmp_peer_t *)val;
        if (peer_filter && peer_filter[0] != '\0' && strcmp(peer_filter, p->peer_ip) != 0)
        {
            continue;
        }

        char t_change[32];
        fmt_time_usec(p->last_change_usec, t_change, sizeof(t_change));
        g_string_append_printf(buf, "  %-16s %-17s %-10u %-10s %-8llu %-8llu\r\n", client->client_id, p->peer_ip,
                               p->peer_as, p->is_up ? "up" : "down", (unsigned long long)p->up_count,
                               (unsigned long long)p->down_count);
        g_string_append_printf(
            buf, "    bgp-id=%s last-change=%s loc-rib-filtered=%s rm(total/pre/post/loc)=%llu/%llu/%llu/%llu\r\n",
            p->bgp_id, t_change, p->is_loc_rib_filtered ? "yes" : "no", (unsigned long long)p->route_monitor_msgs,
            (unsigned long long)p->route_monitor_pre_msgs, (unsigned long long)p->route_monitor_post_msgs,
            (unsigned long long)p->route_monitor_loc_rib_msgs);
        g_string_append_printf(buf, "    upd(total/pre/post/loc)=%llu/%llu/%llu/%llu upd-err=%llu\r\n",
                               (unsigned long long)p->update_msgs, (unsigned long long)p->update_pre_msgs,
                               (unsigned long long)p->update_post_msgs, (unsigned long long)p->update_loc_rib_msgs,
                               (unsigned long long)p->update_parse_errs);
        (*listed)++;
    }
}

/**
 * @brief 处理 "show bmp-server peer [client <id>] [peer <ip>]"
 *
 * group_id=5, cfg_id: 2=client-id, 4=peer-id
 */
static int handle_show_bmp_peer(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char client_id[SBMP_CLIENT_ID_MAX] = {0};
    char peer_ip[SBMP_PEER_IP_MAX] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (entry.cfg_id == 2)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(client_id, sizeof(client_id), "%s", s);
            }
        }
        else if (entry.cfg_id == 4)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(peer_ip, sizeof(peer_ip), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }
    uint32_t listed = 0;

    g_string_append(buf, "BMP Peers\r\n");
    g_string_append(buf, "---------\r\n");
    g_string_append_printf(buf, "  %-16s %-17s %-10s %-10s %-8s %-8s\r\n", "Client", "Peer", "AS", "State", "UpCnt",
                           "DownCnt");

    sbmp_runtime_lock();

    if (client_id[0] != '\0')
    {
        sbmp_client_t *c = sbmp_runtime_find_client_locked(client_id);
        if (!c)
        {
            sbmp_runtime_unlock();
            g_string_free(buf, TRUE);
            sbmp_send_cli_response(msg, "SBMP Error: BMP client not found.\r\n");
            return ERRCODE_FAIL;
        }

        append_peer_rows(buf, c, peer_ip, &listed);
    }
    else
    {
        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init(&iter, g_sbmp_local->client_hash);
        while (g_hash_table_iter_next(&iter, &key, &val))
        {
            (void)key;
            sbmp_client_t *c = (sbmp_client_t *)val;
            append_peer_rows(buf, c, peer_ip, &listed);
        }
    }

    sbmp_runtime_unlock();

    if (listed == 0)
    {
        g_string_append(buf, "  (no peers)\r\n");
    }

    return sbmp_cli_send_chunked_response(msg, buf);
}

typedef struct sbmp_show_route_ctx
{
    GString *buf;
    const char *peer_filter;
    uint32_t listed_heads;
    uint32_t listed_routes;
} sbmp_show_route_ctx_t;

static gboolean sbmp_show_route_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const sbmp_rthead_t *head = (const sbmp_rthead_t *)value;
    sbmp_show_route_ctx_t *ctx = (sbmp_show_route_ctx_t *)user_data;

    if (!head || !ctx)
    {
        return FALSE;
    }

    gboolean printed_head = FALSE;

    GHashTableIter iter;
    gpointer rkey, rval;
    g_hash_table_iter_init(&iter, (GHashTable *)head->route_hash);
    while (g_hash_table_iter_next(&iter, &rkey, &rval))
    {
        (void)rkey;
        const sbmp_route_t *route = (const sbmp_route_t *)rval;
        if (!route)
        {
            continue;
        }

        if (ctx->peer_filter && ctx->peer_filter[0] != '\0')
        {
            char src_str[64];
            net_addr_to_str(&route->source, src_str, sizeof(src_str));
            if (strcmp(src_str, ctx->peer_filter) != 0)
            {
                continue;
            }
        }

        if (!printed_head)
        {
            char nlri_key[BGP_NLRI_KEY_MAX];
            bgp_nlri_to_str(&head->nlri, nlri_key, sizeof(nlri_key));
            g_string_append_printf(ctx->buf, "%s\r\n", nlri_key);
            printed_head = TRUE;
            ctx->listed_heads++;
        }

        char nh[128];
        char lp[16];
        char med[16];
        char as_path[96];

        bgp_nexthop_to_str(&route->nexthop, nh, sizeof(nh));
        snprintf(lp, sizeof(lp), "%s", route->attr.has_local_pref ? "set" : "-");
        snprintf(med, sizeof(med), "%s", route->attr.has_med ? "set" : "-");

        if (route->attr.as_path[0] == '\0')
        {
            snprintf(as_path, sizeof(as_path), "-");
        }
        else
        {
            snprintf(as_path, sizeof(as_path), "%.80s", route->attr.as_path);
        }

        char peer_str[64];
        net_addr_to_str(&route->source, peer_str, sizeof(peer_str));
        g_string_append_printf(ctx->buf, "  - peer=%s nh=%s lp=%s med=%s origin=%s as-path=%s\r\n", peer_str, nh, lp,
                               med, bgp_origin_str(route->attr.origin), as_path);
        ctx->listed_routes++;
    }

    if (printed_head)
    {
        g_string_append(ctx->buf, "\r\n");
    }

    return FALSE;
}

static void append_client_routes(GString *buf, const sbmp_client_t *client, uint16_t afi, uint8_t safi,
                                 sbmp_route_policy_t policy, const char *peer_filter, uint32_t *total_heads,
                                 uint32_t *total_routes)
{
    const sbmp_rib_t *rib = sbmp_client_rib(client, afi, safi, policy);

    if (!rib || sbmp_rib_route_count(rib) == 0)
    {
        return;
    }

    sbmp_show_route_ctx_t ctx;
    ctx.buf = buf;
    ctx.peer_filter = peer_filter;
    ctx.listed_heads = 0;
    ctx.listed_routes = 0;

    g_string_append_printf(ctx.buf, "\r\nClient: %s  Policy: %s\r\n", client->client_id, sbmp_policy_str(policy));
    g_string_append_printf(ctx.buf, "  RIB Heads: %u  Routes: %u\r\n\r\n", sbmp_rib_head_count(rib),
                           sbmp_rib_route_count(rib));

    g_tree_foreach((GTree *)rib->head_tree, sbmp_show_route_head_cb, &ctx);

    if (ctx.listed_routes > 0)
    {
        g_string_append_printf(ctx.buf, "  Listed Heads: %u  Listed Routes: %u\r\n", ctx.listed_heads,
                               ctx.listed_routes);
        *total_heads += ctx.listed_heads;
        *total_routes += ctx.listed_routes;
    }
}

/**
 * @brief 处理 "show bmp-server route af ipv4-unicast|ipv6-unicast [client <id>] [peer <id>] [policy pre|post|loc-rib]"
 *
 * group_id=6, cfg_id: 1=ipv4-unicast, 2=ipv6-unicast, 3=client-id, 4=peer-id, 5=pre, 6=post, 7=loc-rib
 */
static int handle_show_bmp_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint16_t afi = 0;
    uint8_t safi = BGP_SAFI_UNICAST;
    gboolean has_af = FALSE;
    gboolean has_policy = FALSE;
    sbmp_route_policy_t policy = SBMP_ROUTE_POLICY_PRE;
    char client_id[SBMP_CLIENT_ID_MAX] = {0};
    char peer_ip[SBMP_PEER_IP_MAX] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        switch (entry.cfg_id)
        {
            case 1:
                afi = BGP_AFI_IPV4;
                has_af = TRUE;
                break;
            case 2:
                afi = BGP_AFI_IPV6;
                has_af = TRUE;
                break;
            case 3:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(client_id, sizeof(client_id), "%s", s);
                }
                break;
            }
            case 4:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(peer_ip, sizeof(peer_ip), "%s", s);
                }
                break;
            }
            case 5:
                policy = SBMP_ROUTE_POLICY_PRE;
                has_policy = TRUE;
                break;
            case 6:
                policy = SBMP_ROUTE_POLICY_POST;
                has_policy = TRUE;
                break;
            case 7:
                policy = SBMP_ROUTE_POLICY_LOC_RIB;
                has_policy = TRUE;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_af)
    {
        sbmp_send_cli_response(msg,
                               "SBMP Error: Missing address-family. Use 'af ipv4-unicast' or 'af ipv6-unicast'.\r\n");
        return ERRCODE_FAIL;
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }
    uint32_t total_heads = 0;
    uint32_t total_routes = 0;

    g_string_append_printf(buf, "BMP Routes (AF: %s, Policy: %s)\r\n",
                           afi == BGP_AFI_IPV4 ? "ipv4-unicast" : "ipv6-unicast",
                           has_policy ? sbmp_policy_str(policy) : "all");
    g_string_append(buf, "============================================================\r\n");

    sbmp_runtime_lock();

    if (client_id[0] != '\0')
    {
        sbmp_client_t *c = sbmp_runtime_find_client_locked(client_id);
        if (!c)
        {
            sbmp_runtime_unlock();
            g_string_free(buf, TRUE);
            sbmp_send_cli_response(msg, "SBMP Error: BMP client not found.\r\n");
            return ERRCODE_FAIL;
        }

        if (has_policy)
        {
            append_client_routes(buf, c, afi, safi, policy, peer_ip, &total_heads, &total_routes);
        }
        else
        {
            append_client_routes(buf, c, afi, safi, SBMP_ROUTE_POLICY_PRE, peer_ip, &total_heads, &total_routes);
            append_client_routes(buf, c, afi, safi, SBMP_ROUTE_POLICY_POST, peer_ip, &total_heads, &total_routes);
            append_client_routes(buf, c, afi, safi, SBMP_ROUTE_POLICY_LOC_RIB, peer_ip, &total_heads, &total_routes);
        }
    }
    else
    {
        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init(&iter, g_sbmp_local->client_hash);
        while (g_hash_table_iter_next(&iter, &key, &val))
        {
            (void)key;
            sbmp_client_t *c = (sbmp_client_t *)val;
            if (has_policy)
            {
                append_client_routes(buf, c, afi, safi, policy, peer_ip, &total_heads, &total_routes);
            }
            else
            {
                append_client_routes(buf, c, afi, safi, SBMP_ROUTE_POLICY_PRE, peer_ip, &total_heads, &total_routes);
                append_client_routes(buf, c, afi, safi, SBMP_ROUTE_POLICY_POST, peer_ip, &total_heads, &total_routes);
                append_client_routes(buf, c, afi, safi, SBMP_ROUTE_POLICY_LOC_RIB, peer_ip, &total_heads,
                                     &total_routes);
            }
        }
    }

    sbmp_runtime_unlock();

    if (total_routes == 0)
    {
        g_string_append(buf, "  (no routes)\r\n");
    }
    else
    {
        g_string_append_printf(buf, "\r\nTotal Listed Heads: %u  Total Listed Routes: %u\r\n", total_heads,
                               total_routes);
    }

    return sbmp_cli_send_chunked_response(msg, buf);
}

// ============================================================================
// 消息分发入口
// ============================================================================

int sbmp_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_chunk_stream_reset(&g_sbmp_local->show_stream);

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("SBMP: Payload parsing failed");
        sbmp_send_cli_response(msg, "SBMP Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("SBMP: Received TLV payload (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case SBMP_CLI_GROUP_ID_BMP_SERVER:
            result = handle_bmp_server(msg, &parser);
            break;
        case SBMP_CLI_GROUP_ID_SERVER_PORT:
            result = handle_server_port(msg, &parser);
            break;
        case SBMP_CLI_GROUP_ID_SHOW_STATUS:
            result = handle_show_bmp_server(msg, &parser);
            break;
        case SBMP_CLI_GROUP_ID_SHOW_CLIENT:
            result = handle_show_bmp_client(msg, &parser);
            break;
        case SBMP_CLI_GROUP_ID_SHOW_PEER:
            result = handle_show_bmp_peer(msg, &parser);
            break;
        case SBMP_CLI_GROUP_ID_SHOW_ROUTE:
            result = handle_show_bmp_route(msg, &parser);
            break;
        default:
            LOG_WARN("SBMP: Unknown group_id: %u", parser.group_id);
            sbmp_send_cli_response(msg, "SBMP Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}

int sbmp_cli_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_sbmp_local->show_stream, sbmp_local_ipc_ctx(), DEV_MODULE_ID_SBMP, msg);
}

void sbmp_cli_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_sbmp_local->show_stream);
}
