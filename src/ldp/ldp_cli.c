/**
 * @file   ldp_cli.c
 * @brief  LDP CLI 配置命令处理（IPC 线程）
 *
 * 单实例 LDP：协议视图命令直接落 DB；接口视图命令绑定到具体 ifname。
 * M1 阶段不向 worker 派发 apply（worker 还没有内存态），仅保证持久化。
 *
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_cli.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "cli.h"
#include "errcode.h"
#include "if.h"
#include "ldp.h"
#include "ldp_db.h"
#include "ldp_main.h"
#include "log.h"
#include "work/ldp_worker.h"

#define LDP_IF_LOOP_ID_MIN 1u
#define LDP_IF_LOOP_ID_MAX 1024u

static void send_resp_typed(dev_ipc_message_t *msg, uint32_t msg_type, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(msg_type, DEV_MODULE_ID_LDP, msg->src_module_id, msg->request_id,
                                                     resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(ldp_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    send_resp_typed(msg, CLI_MSG_TYPE_RESP, text);
}

static const char *if_ctx_idx_to_name(uint32_t if_idx)
{
    switch (if_idx)
    {
        case 1:
            return "GE-1";
        case 2:
            return "GE-2";
        case 3:
            return "GE-3";
        case 4:
            return "GE-4";
        case 5:
            return "GE-5";
        case 6:
            return "GE-6";
        case 7:
            return "GE-7";
        case 8:
            return "GE-8";
        default:
            return NULL;
    }
}

static int dispatch_proto_apply(void)
{
    ldp_proto_cfg_t cfg;
    if (ldp_db_get_proto_cfg(&cfg) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    ldp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = LDP_APPLY_OP_PROTO_SET;
    apply.u.proto = cfg;
    if (ldp_worker_dispatch_apply(&apply) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    return apply.rc;
}

static int dispatch_if_apply(const char *ifname, int enabled)
{
    ldp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    if (enabled)
    {
        ldp_if_cfg_t cfg;
        if (ldp_db_get_interface(ifname, &cfg) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
        apply.op = LDP_APPLY_OP_IF_SET;
        apply.u.if_set = cfg;
    }
    else
    {
        apply.op = LDP_APPLY_OP_IF_DEL;
        g_strlcpy(apply.u.if_del.ifname, ifname, sizeof(apply.u.if_del.ifname));
    }
    if (ldp_worker_dispatch_apply(&apply) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    return apply.rc;
}

static int handle_proto_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    /* group 1 命令暂无附加参数，遍历主要是消费 entries */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }

    if (ldp_db_set_proto_admin(is_no ? 0u : 1u) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LDP Error: Failed to persist admin state\r\n");
        return ERRCODE_FAIL;
    }
    (void)dispatch_proto_apply();

    if (is_no)
    {
        /* `no ldp`：admin=0，业务停摆，进程自退出让 DEV 回到 on-demand 待命。
         * 接口级配置在 DB 中保留，下次 `ldp` 启动时由 db_restore 还原。
         * kill(getpid, SIGTERM) 触发 ldp_proc.c 的 shutdown_handler 优雅退出。 */
        send_resp_typed(msg, CLI_MSG_TYPE_RESP_EXITING, "LDP: admin disabled, process exiting.\r\n");
        kill(getpid(), SIGTERM);
        return ERRCODE_SUCCESS;
    }

    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static uint32_t parse_ipv4_dotted(const char *s)
{
    if (!s)
    {
        return 0u;
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
    {
        return 0u;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255)
    {
        return 0u;
    }
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

static int handle_lsr_id_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t lsr_id = 0u;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            lsr_id = parse_ipv4_dotted(s);
        }
        cli_tlv_entry_free(&entry);
    }

    if (is_no)
    {
        lsr_id = 0u;
    }
    else if (lsr_id == 0u)
    {
        send_resp(msg, "LDP Error: Invalid LSR-ID\r\n");
        return ERRCODE_FAIL;
    }

    if (ldp_db_set_lsr_id(lsr_id) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LDP Error: Failed to persist LSR-ID\r\n");
        return ERRCODE_FAIL;
    }
    (void)dispatch_proto_apply();
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int handle_timers_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    int has_hello_kw = 0;
    int has_hold_kw = 0;
    int has_keepalive_kw = 0;
    uint32_t value_ms = 0u;
    int has_value = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            has_hello_kw = 1;
        }
        else if (entry.cfg_id == 2)
        {
            has_hold_kw = 1;
        }
        else if (entry.cfg_id == 3)
        {
            has_keepalive_kw = 1;
        }
        else if (entry.cfg_id == 4)
        {
            int64_t v = cli_tlv_entry_get_int(&entry);
            if (v >= (int64_t)LDP_MIN_HELLO_INTERVAL_MS && v <= (int64_t)LDP_MAX_HELLO_INTERVAL_MS)
            {
                value_ms = (uint32_t)v;
                has_value = 1;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    int kw_count = has_hello_kw + has_hold_kw + has_keepalive_kw;
    if (kw_count != 1)
    {
        send_resp(msg, "LDP Error: Invalid timer command\r\n");
        return ERRCODE_FAIL;
    }
    if (!is_no && !has_value)
    {
        send_resp(msg, "LDP Error: Missing timer value\r\n");
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    if (has_hello_kw)
    {
        rc = ldp_db_set_hello_interval(is_no ? LDP_DEFAULT_HELLO_INTERVAL_MS : value_ms);
    }
    else if (has_hold_kw)
    {
        rc = ldp_db_set_hold_time(is_no ? LDP_DEFAULT_HOLD_TIME_MS : value_ms);
    }
    else if (has_keepalive_kw)
    {
        rc = ldp_db_set_keepalive(is_no ? LDP_DEFAULT_KEEPALIVE_INTERVAL_MS : value_ms);
    }

    if (rc != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LDP Error: Failed to persist timer\r\n");
        return ERRCODE_FAIL;
    }
    (void)dispatch_proto_apply();
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int resolve_ifname_from_ctx(cli_tlv_parser_t *parser, char *ifname, size_t ifname_len)
{
    uint32_t if_idx = 0u;
    uint32_t loop_id = 0u;

    cli_tlv_rewind(parser);
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_IF_IDX)
            {
                if_idx = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            else if (entry.cfg_id == CLI_CTX_ID_IF_LOOP_IDX)
            {
                loop_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
        }
        cli_tlv_entry_free(&entry);
    }
    cli_tlv_rewind(parser);

    if (loop_id >= LDP_IF_LOOP_ID_MIN && loop_id <= LDP_IF_LOOP_ID_MAX)
    {
        snprintf(ifname, ifname_len, "loop%u", loop_id);
        return ERRCODE_SUCCESS;
    }
    const char *name = if_ctx_idx_to_name(if_idx);
    if (!name)
    {
        return ERRCODE_FAIL;
    }
    g_strlcpy(ifname, name, ifname_len);
    return ERRCODE_SUCCESS;
}

static int handle_if_view_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char ifname[IF_LOGICAL_NAME_MAX] = {0};
    if (resolve_ifname_from_ctx(parser, ifname, sizeof(ifname)) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LDP Error: No interface context\r\n");
        return ERRCODE_FAIL;
    }

    int has_enable_kw = 0;
    int has_hello_kw = 0;
    int has_hold_kw = 0;
    uint32_t value_ms = 0u;
    int has_value = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            has_enable_kw = 1;
        }
        else if (entry.cfg_id == 2)
        {
            has_hello_kw = 1;
        }
        else if (entry.cfg_id == 3)
        {
            has_hold_kw = 1;
        }
        else if (entry.cfg_id == 4)
        {
            int64_t v = cli_tlv_entry_get_int(&entry);
            if (v >= (int64_t)LDP_MIN_HELLO_INTERVAL_MS && v <= (int64_t)LDP_MAX_HELLO_INTERVAL_MS)
            {
                value_ms = (uint32_t)v;
                has_value = 1;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    int kw_count = has_enable_kw + has_hello_kw + has_hold_kw;
    if (kw_count != 1)
    {
        send_resp(msg, "LDP Error: Invalid interface command\r\n");
        return ERRCODE_FAIL;
    }

    ldp_if_cfg_t cfg;
    int have_existing = (ldp_db_get_interface(ifname, &cfg) == ERRCODE_SUCCESS);
    if (!have_existing)
    {
        memset(&cfg, 0, sizeof(cfg));
        g_strlcpy(cfg.ifname, ifname, sizeof(cfg.ifname));
    }

    if (has_enable_kw)
    {
        if (is_no)
        {
            if (have_existing)
            {
                (void)ldp_db_del_interface(ifname);
            }
            (void)dispatch_if_apply(ifname, 0);
            send_resp(msg, "");
            return ERRCODE_SUCCESS;
        }
        cfg.enabled = 1u;
    }
    else
    {
        if (!have_existing || !cfg.enabled)
        {
            if (is_no)
            {
                send_resp(msg, "");
                return ERRCODE_SUCCESS;
            }
            send_resp(msg, "LDP Error: Interface LDP not enabled\r\n");
            return ERRCODE_FAIL;
        }

        if (has_hello_kw)
        {
            cfg.hello_interval_ms = is_no ? 0u : value_ms;
        }
        else if (has_hold_kw)
        {
            cfg.hold_time_ms = is_no ? 0u : value_ms;
        }

        if (!is_no && !has_value)
        {
            send_resp(msg, "LDP Error: Missing value\r\n");
            return ERRCODE_FAIL;
        }
    }

    if (ldp_db_set_interface(ifname, &cfg) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LDP Error: Failed to persist interface config\r\n");
        return ERRCODE_FAIL;
    }
    (void)dispatch_if_apply(ifname, cfg.enabled ? 1 : 0);
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

int ldp_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        send_resp(msg, "LDP Error: Invalid command payload\r\n");
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    switch (parser.group_id)
    {
        case LDP_CLI_GROUP_ID_PROTO:
            rc = handle_proto_cmd(msg, &parser);
            break;
        case LDP_CLI_GROUP_ID_LSR_ID:
            rc = handle_lsr_id_cmd(msg, &parser);
            break;
        case LDP_CLI_GROUP_ID_TIMERS:
            rc = handle_timers_cmd(msg, &parser);
            break;
        case LDP_CLI_GROUP_ID_IF_VIEW:
            rc = handle_if_view_cmd(msg, &parser);
            break;
        default:
            send_resp(msg, "LDP Error: Unknown command group\r\n");
            rc = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return rc;
}
