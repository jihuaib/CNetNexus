/**
 * @file   isis_cli.c
 * @brief  ISIS CLI 配置命令处理（IPC 线程）
 * @author jhb
 * @date   2026/04/11
 */
#include "isis_cli.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "isis.h"
#include "isis_db.h"
#include "isis_main.h"
#include "isis_worker.h"
#include "log.h"
#include "srv6.h"
#include "vrf.h"

/* 表名重复但保持就近：避免引 isis_db_internal.h（仅 db/ 子目录可见） */
#define ISIS_TABLE_INSTANCE_NAME "isis_instance"

#define IF_LOOP_ID_MIN 1u
#define IF_LOOP_ID_MAX 1024u

static void send_resp_typed(dev_ipc_message_t *msg, uint32_t msg_type, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(msg_type, DEV_MODULE_ID_ISIS, msg->src_module_id, msg->request_id,
                                                     resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(isis_local_ipc_ctx(), resp);
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

static int dispatch_and_respond_ex(dev_ipc_message_t *msg, isis_apply_cmd_t *apply, gboolean respond_on_noop)
{
    if (!apply || isis_worker_dispatch_apply(apply) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "ISIS Error: Server unavailable\r\n");
        return -1;
    }
    if (apply->rc == ISIS_APPLY_RC_NOOP)
    {
        if (respond_on_noop)
        {
            send_resp(msg, "");
        }
        return 0;
    }
    if (apply->rc != ISIS_APPLY_RC_OK)
    {
        char buf[300];
        g_snprintf(buf, sizeof(buf), "%s\r\n", apply->errmsg[0] != '\0' ? apply->errmsg : "ISIS Error: Apply failed");
        send_resp(msg, buf);
        return -1;
    }
    return 1;
}

/**
 * 派发 apply 并按 BGP 风格处理响应。返回：
 *    1 = apply.rc == OK，调用方需继续写 DB，最后 send_resp("")
 *    0 = apply.rc == NOOP，响应已发空串，调用方直接 return ERRCODE_SUCCESS
 *   -1 = apply.rc == FAIL 或派发失败，响应已发 errmsg，调用方直接 return ERRCODE_FAIL
 */
static int dispatch_and_respond(dev_ipc_message_t *msg, isis_apply_cmd_t *apply)
{
    return dispatch_and_respond_ex(msg, apply, TRUE);
}

static int handle_instance_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t tag = 0u;
    uint32_t ctx_tag = 0u;
    char requested_vrf[IF_VRF_NAME_MAX] = VRF_PUBLIC_VRF_NAME;
    gboolean vrf_specified = FALSE;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
            {
                ctx_tag = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 2)
        {
            int64_t v = cli_tlv_entry_get_int(&entry);
            if (v > 0 && v <= 0xFFFFFFFFll)
            {
                tag = (uint32_t)v;
            }
        }
        else if (entry.cfg_id == 3)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                g_strlcpy(requested_vrf, text, sizeof(requested_vrf));
                vrf_specified = TRUE;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (is_no)
    {
        if (tag == 0u)
        {
            tag = ctx_tag;
        }
        if (tag == 0u)
        {
            send_resp(msg, "ISIS Error: Missing instance tag\r\n");
            return ERRCODE_FAIL;
        }

        isis_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = ISIS_APPLY_OP_INSTANCE_DEL;
        apply.u.instance_del.tag = tag;
        /*
         * 删除实例时 NOOP 只代表内存中没有该实例，DB 中仍可能残留
         * revive 配置。因此暂不回复，继续统一执行 DB 删除和全表空检查。
         */
        int dr = dispatch_and_respond_ex(msg, &apply, FALSE);
        if (dr < 0)
        {
            return ERRCODE_FAIL;
        }

        if (isis_db_del_instance(tag) != ERRCODE_SUCCESS)
        {
            send_resp(msg, "ISIS Error: Failed to delete instance from DB\r\n");
            return ERRCODE_FAIL;
        }
        /* 最后一个实例被删 → revive_table（isis_instance）空 → 进程自退出，
         * 让 DEV 回到 on-demand 待命；下次配 isis <tag> 时 wait_module_ready 重新 fork。 */
        gboolean has_more = FALSE;
        if (db_rpc_exists(isis_local_ipc_ctx(), ISIS_TABLE_INSTANCE_NAME, NULL, &has_more) == ERRCODE_SUCCESS &&
            !has_more)
        {
            send_resp_typed(msg, CLI_MSG_TYPE_RESP_EXITING, "ISIS: last instance removed, process exiting.\r\n");
            kill(getpid(), SIGTERM);
            return ERRCODE_SUCCESS;
        }
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (tag == 0u)
    {
        send_resp(msg, "ISIS Error: Missing instance tag\r\n");
        return ERRCODE_FAIL;
    }

    isis_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    uint32_t vrf_id = VRF_PUBLIC_VRF_ID;
    char vrf_name[IF_VRF_NAME_MAX] = VRF_PUBLIC_VRF_NAME;
    int exists = isis_db_get_instance_vrf(tag, &vrf_id, vrf_name, sizeof(vrf_name)) == ERRCODE_SUCCESS;
    if (exists && vrf_specified && strcmp(vrf_name, requested_vrf) != 0)
    {
        char buf[180];
        g_snprintf(buf, sizeof(buf), "ISIS Error: process %u is already bound to VRF %s\r\n", tag, vrf_name);
        send_resp(msg, buf);
        return ERRCODE_FAIL;
    }
    if (!exists)
    {
        g_strlcpy(vrf_name, requested_vrf, sizeof(vrf_name));
        if (isis_db_resolve_vrf(vrf_name, &vrf_id) != ERRCODE_SUCCESS)
        {
            send_resp(msg, "ISIS Error: VRF does not exist\r\n");
            return ERRCODE_FAIL;
        }
    }
    apply.op = ISIS_APPLY_OP_INSTANCE_SET;
    apply.u.instance_set.tag = tag;
    apply.u.instance_set.vrf_id = vrf_id;
    g_strlcpy(apply.u.instance_set.vrf_name, vrf_name, sizeof(apply.u.instance_set.vrf_name));
    apply.u.instance_set.admin_up = 1u;
    apply.u.instance_set.is_type = ISIS_IS_TYPE_LEVEL_1_2;
    apply.u.instance_set.net[0] = '\0';
    int dr = dispatch_and_respond(msg, &apply);
    if (dr < 0)
    {
        return ERRCODE_FAIL;
    }
    if (dr == 0)
    {
        return ERRCODE_SUCCESS;
    }

    if (isis_db_set_instance(tag, vrf_id, vrf_name) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "ISIS Error: Failed to persist instance\r\n");
        return ERRCODE_FAIL;
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int handle_net_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t tag = 0u;
    char net[ISIS_NET_STR_MAX] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
            {
                tag = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 1)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                g_strlcpy(net, s, sizeof(net));
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (tag == 0u || (!is_no && net[0] == '\0'))
    {
        send_resp(msg, "ISIS Error: Missing tag or net\r\n");
        return ERRCODE_FAIL;
    }

    isis_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = ISIS_APPLY_OP_NET_SET;
    apply.u.net_set.tag = tag;
    g_strlcpy(apply.u.net_set.net, is_no ? "" : net, sizeof(apply.u.net_set.net));
    int dr = dispatch_and_respond(msg, &apply);
    if (dr < 0)
    {
        return ERRCODE_FAIL;
    }
    if (dr == 0)
    {
        return ERRCODE_SUCCESS;
    }

    if (isis_db_set_net(tag, is_no ? "" : net) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "ISIS Error: Failed to persist net\r\n");
        return ERRCODE_FAIL;
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int handle_is_type_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t tag = 0u;
    uint8_t is_type = 0u;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
            {
                tag = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 1)
        {
            is_type = ISIS_IS_TYPE_LEVEL_1;
        }
        else if (entry.cfg_id == 2)
        {
            is_type = ISIS_IS_TYPE_LEVEL_2;
        }
        else if (entry.cfg_id == 3)
        {
            is_type = ISIS_IS_TYPE_LEVEL_1_2;
        }

        cli_tlv_entry_free(&entry);
    }

    if (tag == 0u || is_type == 0u)
    {
        send_resp(msg, "ISIS Error: Missing tag or is-type\r\n");
        return ERRCODE_FAIL;
    }

    isis_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = ISIS_APPLY_OP_IS_TYPE_SET;
    apply.u.is_type_set.tag = tag;
    apply.u.is_type_set.is_type = is_type;
    int dr = dispatch_and_respond(msg, &apply);
    if (dr < 0)
    {
        return ERRCODE_FAIL;
    }
    if (dr == 0)
    {
        return ERRCODE_SUCCESS;
    }

    if (isis_db_set_is_type(tag, is_type) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "ISIS Error: Failed to persist is-type\r\n");
        return ERRCODE_FAIL;
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int handle_cost_style_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t tag = 0u;
    uint8_t cost_style = 0u;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
            {
                tag = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 1)
        {
            cost_style = ISIS_COST_STYLE_NARROW;
        }
        else if (entry.cfg_id == 2)
        {
            cost_style = ISIS_COST_STYLE_WIDE;
        }

        cli_tlv_entry_free(&entry);
    }

    if (tag == 0u || cost_style == 0u)
    {
        send_resp(msg, "ISIS Error: Missing tag or cost-style\r\n");
        return ERRCODE_FAIL;
    }

    isis_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = ISIS_APPLY_OP_COST_STYLE_SET;
    apply.u.cost_style_set.tag = tag;
    apply.u.cost_style_set.cost_style = cost_style;
    int dr = dispatch_and_respond(msg, &apply);
    if (dr < 0)
    {
        return ERRCODE_FAIL;
    }
    if (dr == 0)
    {
        return ERRCODE_SUCCESS;
    }

    if (isis_db_set_cost_style(tag, cost_style) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "ISIS Error: Failed to persist cost-style\r\n");
        return ERRCODE_FAIL;
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int handle_af_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int enable = ((parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) == 0);
    uint32_t tag = 0u;
    uint16_t afi = 0u;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
            {
                tag = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 2)
        {
            afi = ISIS_AFI_IPV4;
        }
        else if (entry.cfg_id == 3)
        {
            afi = ISIS_AFI_IPV6;
        }
        cli_tlv_entry_free(&entry);
    }

    if (tag == 0u || afi == 0u)
    {
        send_resp(msg, "ISIS Error: Missing tag or AFI\r\n");
        return ERRCODE_FAIL;
    }

    isis_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = enable ? ISIS_APPLY_OP_AF_SET : ISIS_APPLY_OP_AF_DEL;
    apply.u.af_set.tag = tag;
    apply.u.af_set.afi = afi;
    int dr = dispatch_and_respond(msg, &apply);
    if (dr < 0)
    {
        return ERRCODE_FAIL;
    }
    if (dr == 0)
    {
        return ERRCODE_SUCCESS;
    }

    if (isis_db_set_af(tag, afi, enable) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "ISIS Error: Failed to persist AF setting\r\n");
        return ERRCODE_FAIL;
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int handle_srv6_locator_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t tag = 0u;
    char locator[SRV6_LOCATOR_NAME_MAX] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
            {
                tag = cli_tlv_entry_get_ctx_uint32(&entry);
            }
        }
        else if (entry.cfg_id == 1)
        {
            const char *name = cli_tlv_entry_get_text(&entry);
            if (name)
            {
                g_strlcpy(locator, name, sizeof(locator));
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (tag == 0u || (!is_no && locator[0] == '\0'))
    {
        send_resp(msg, "ISIS Error: Missing instance tag or SRv6 locator name\r\n");
        return ERRCODE_FAIL;
    }
    if (!is_no)
    {
        if (dev_ipc_wait_module_ready(isis_local_ipc_ctx(), DEV_MODULE_ID_SRV6, DEV_IPC_WAIT_READY_MS) !=
                ERRCODE_SUCCESS ||
            dev_ipc_wait_connected(isis_local_ipc_ctx(), DEV_MODULE_ID_SRV6, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS ||
            srv6_rpc_locator_exists(isis_local_ipc_ctx(), locator, SRV6_RPC_DEFAULT_TIMEOUT_MS) != ERRCODE_SUCCESS)
        {
            send_resp(msg, "ISIS Error: SRv6 locator does not exist or the SRV6 module is unavailable\r\n");
            return ERRCODE_FAIL;
        }
    }

    char old_locator[SRV6_LOCATOR_NAME_MAX] = {0};
    if (isis_db_get_srv6_locator(tag, old_locator, sizeof(old_locator)) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "ISIS Error: Failed to read the current SRv6 locator setting\r\n");
        return ERRCODE_FAIL;
    }

    isis_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = ISIS_APPLY_OP_SRV6_LOCATOR_SET;
    apply.u.srv6_locator_set.tag = tag;
    g_strlcpy(apply.u.srv6_locator_set.locator, is_no ? "" : locator, sizeof(apply.u.srv6_locator_set.locator));
    /* A runtime NOOP must still repair persistent state after a previous
     * DB-commit/rollback split. Suppress the helper's early response and
     * always drive the idempotent DB update below. */
    int dr = dispatch_and_respond_ex(msg, &apply, FALSE);
    if (dr < 0)
    {
        return ERRCODE_FAIL;
    }
    if (isis_db_set_srv6_locator(tag, is_no ? "" : locator) != ERRCODE_SUCCESS)
    {
        isis_apply_cmd_t rollback;
        memset(&rollback, 0, sizeof(rollback));
        rollback.op = ISIS_APPLY_OP_SRV6_LOCATOR_SET;
        rollback.u.srv6_locator_set.tag = tag;
        g_strlcpy(rollback.u.srv6_locator_set.locator, old_locator, sizeof(rollback.u.srv6_locator_set.locator));
        if (isis_worker_dispatch_apply(&rollback) != ERRCODE_SUCCESS ||
            (rollback.rc != ISIS_APPLY_RC_OK && rollback.rc != ISIS_APPLY_RC_NOOP))
        {
            LOG_ERROR("ISIS: failed to roll back SRv6 locator runtime state for process %u", tag);
        }
        send_resp(msg, "ISIS Error: Failed to persist the SRv6 locator setting\r\n");
        return ERRCODE_FAIL;
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static void if_cfg_set_defaults(isis_if_cfg_t *cfg, const char *ifname)
{
    if (!cfg)
    {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    if (ifname)
    {
        g_strlcpy(cfg->ifname, ifname, sizeof(cfg->ifname));
    }
    cfg->v4.enabled = 0u;
    cfg->v4.metric = ISIS_DEFAULT_IF_METRIC;
    cfg->v4.hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
    cfg->v4.hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
    cfg->v4.passive = ISIS_DEFAULT_IF_PASSIVE;

    cfg->v6.enabled = 0u;
    cfg->v6.metric = ISIS_DEFAULT_IF_METRIC;
    cfg->v6.hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
    cfg->v6.hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
    cfg->v6.passive = ISIS_DEFAULT_IF_PASSIVE;
    cfg->last_hello_tx_msec = 0u;
}

/**
 * 派发 IF_SET 并按 BGP 风格处理响应。返回与 dispatch_and_respond 相同：
 *    1 = OK 调用方写 DB
 *    0 = NOOP 调用方直接 return ERRCODE_SUCCESS
 *   -1 = FAIL 调用方直接 return ERRCODE_FAIL
 */
static int dispatch_if_set(dev_ipc_message_t *msg, uint32_t tag, const isis_if_cfg_t *cfg)
{
    if (!cfg)
    {
        return -1;
    }
    isis_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = ISIS_APPLY_OP_IF_SET;
    apply.u.if_set.tag = tag;
    apply.u.if_set.cfg = *cfg;
    return dispatch_and_respond(msg, &apply);
}

typedef enum isis_if_action
{
    ISIS_IF_ACTION_NONE = 0,
    ISIS_IF_ACTION_ENABLE = 1,
    ISIS_IF_ACTION_METRIC = 2,
    ISIS_IF_ACTION_HELLO_INTERVAL = 3,
    ISIS_IF_ACTION_HOLD_MULTIPLIER = 4,
    ISIS_IF_ACTION_PASSIVE = 5,
} isis_if_action_t;

static int handle_if_view_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t tag = 0u;
    uint16_t cmd_afi = ISIS_AFI_IPV4;
    uint32_t if_idx = 0u;
    uint32_t loop_id = 0u;
    int has_enable = 0;
    int has_metric_kw = 0;
    int has_hello_kw = 0;
    int has_hold_kw = 0;
    int has_passive_kw = 0;
    uint32_t metric = ISIS_DEFAULT_IF_METRIC;
    int has_metric_val = 0;
    uint16_t hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
    int has_hello_val = 0;
    uint8_t hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
    int has_hold_val = 0;

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
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 2)
        {
            cmd_afi = ISIS_AFI_IPV6;
        }
        else if (entry.cfg_id == 3)
        {
            has_enable = 1;
        }
        else if (entry.cfg_id == 4)
        {
            has_metric_kw = 1;
        }
        else if (entry.cfg_id == 5)
        {
            int64_t v = cli_tlv_entry_get_int(&entry);
            if (v > 0 && v <= 0xFFFFFFFFll)
            {
                tag = (uint32_t)v;
            }
        }
        else if (entry.cfg_id == 6)
        {
            int64_t v = cli_tlv_entry_get_int(&entry);
            if (v >= 1 && v <= ISIS_MAX_IF_METRIC)
            {
                metric = (uint32_t)v;
                has_metric_val = 1;
            }
        }
        else if (entry.cfg_id == 7)
        {
            has_hello_kw = 1;
        }
        else if (entry.cfg_id == 8)
        {
            int64_t v = cli_tlv_entry_get_int(&entry);
            if (v >= 1 && v <= ISIS_MAX_HELLO_INTERVAL)
            {
                hello_interval = (uint16_t)v;
                has_hello_val = 1;
            }
        }
        else if (entry.cfg_id == 9)
        {
            has_hold_kw = 1;
        }
        else if (entry.cfg_id == 10)
        {
            int64_t v = cli_tlv_entry_get_int(&entry);
            if (v >= 1 && v <= ISIS_MAX_HOLD_MULTIPLIER)
            {
                hold_multiplier = (uint8_t)v;
                has_hold_val = 1;
            }
        }
        else if (entry.cfg_id == 11)
        {
            has_passive_kw = 1;
        }
        cli_tlv_entry_free(&entry);
    }

    char ifname[IF_LOGICAL_NAME_MAX] = {0};
    if (loop_id >= IF_LOOP_ID_MIN && loop_id <= IF_LOOP_ID_MAX)
    {
        snprintf(ifname, sizeof(ifname), "loop%u", loop_id);
    }
    else
    {
        const char *name = if_ctx_idx_to_name(if_idx);
        if (name)
        {
            g_strlcpy(ifname, name, sizeof(ifname));
        }
    }

    if (ifname[0] == '\0')
    {
        send_resp(msg, "ISIS Error: No interface context\r\n");
        return ERRCODE_FAIL;
    }

    if (tag == 0u)
    {
        send_resp(msg, "ISIS Error: Missing instance tag\r\n");
        return ERRCODE_FAIL;
    }

    int af_enabled = 0;
    if (isis_db_is_af_enabled(tag, cmd_afi, &af_enabled) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "ISIS Error: Instance tag not found\r\n");
        return ERRCODE_FAIL;
    }
    if (!af_enabled && !is_no)
    {
        send_resp(msg, "ISIS Error: Instance AF is not enabled\r\n");
        return ERRCODE_FAIL;
    }

    int action_count = 0;
    action_count += has_enable ? 1 : 0;
    action_count += has_metric_kw ? 1 : 0;
    action_count += has_hello_kw ? 1 : 0;
    action_count += has_hold_kw ? 1 : 0;
    action_count += has_passive_kw ? 1 : 0;

    if (action_count != 1)
    {
        send_resp(msg, "ISIS Error: Invalid interface command\r\n");
        return ERRCODE_FAIL;
    }

    isis_if_action_t action = ISIS_IF_ACTION_NONE;
    if (has_enable)
    {
        action = ISIS_IF_ACTION_ENABLE;
    }
    else if (has_metric_kw)
    {
        action = ISIS_IF_ACTION_METRIC;
    }
    else if (has_hello_kw)
    {
        action = ISIS_IF_ACTION_HELLO_INTERVAL;
    }
    else if (has_hold_kw)
    {
        action = ISIS_IF_ACTION_HOLD_MULTIPLIER;
    }
    else if (has_passive_kw)
    {
        action = ISIS_IF_ACTION_PASSIVE;
    }

    isis_if_cfg_t cfg;
    int has_cfg = (isis_db_get_interface_cfg(tag, ifname, &cfg) == ERRCODE_SUCCESS);
    if (!has_cfg)
    {
        if_cfg_set_defaults(&cfg, ifname);
    }
    else if (cfg.ifname[0] == '\0')
    {
        g_strlcpy(cfg.ifname, ifname, sizeof(cfg.ifname));
    }

    isis_if_af_cfg_t *af_cfg = isis_if_cfg_af(&cfg, cmd_afi);
    if (!af_cfg)
    {
        send_resp(msg, "ISIS Error: Invalid AF\r\n");
        return ERRCODE_FAIL;
    }

    if (action != ISIS_IF_ACTION_ENABLE && !has_cfg && !is_no)
    {
        send_resp(msg, "ISIS Error: Interface ISIS not enabled\r\n");
        return ERRCODE_FAIL;
    }
    if (action != ISIS_IF_ACTION_ENABLE && !has_cfg && is_no)
    {
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (action != ISIS_IF_ACTION_ENABLE && !af_cfg->enabled && !is_no)
    {
        send_resp(msg, "ISIS Error: Interface ISIS AF not enabled\r\n");
        return ERRCODE_FAIL;
    }
    if (action != ISIS_IF_ACTION_ENABLE && !af_cfg->enabled && is_no)
    {
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (action == ISIS_IF_ACTION_ENABLE)
    {
        if (is_no)
        {
            if (!has_cfg || !af_cfg->enabled)
            {
                send_resp(msg, "");
                return ERRCODE_SUCCESS;
            }

            af_cfg->enabled = 0u;
            af_cfg->metric = ISIS_DEFAULT_IF_METRIC;
            af_cfg->hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
            af_cfg->hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
            af_cfg->passive = ISIS_DEFAULT_IF_PASSIVE;

            if (!isis_if_cfg_any_enabled(&cfg))
            {
                isis_apply_cmd_t apply;
                memset(&apply, 0, sizeof(apply));
                apply.op = ISIS_APPLY_OP_IF_DEL;
                apply.u.if_del.tag = tag;
                g_strlcpy(apply.u.if_del.ifname, ifname, sizeof(apply.u.if_del.ifname));
                int dr = dispatch_and_respond(msg, &apply);
                if (dr < 0)
                {
                    return ERRCODE_FAIL;
                }
                if (dr == 0)
                {
                    return ERRCODE_SUCCESS;
                }
                if (isis_db_del_interface(tag, ifname) != ERRCODE_SUCCESS)
                {
                    send_resp(msg, "ISIS Error: Failed to delete interface config\r\n");
                    return ERRCODE_FAIL;
                }
            }
            else
            {
                int dr = dispatch_if_set(msg, tag, &cfg);
                if (dr < 0)
                {
                    return ERRCODE_FAIL;
                }
                if (dr == 0)
                {
                    return ERRCODE_SUCCESS;
                }
                if (isis_db_set_interface_cfg(tag, ifname, &cfg) != ERRCODE_SUCCESS)
                {
                    send_resp(msg, "ISIS Error: Failed to persist interface config\r\n");
                    return ERRCODE_FAIL;
                }
            }
            send_resp(msg, "");
            return ERRCODE_SUCCESS;
        }

        af_cfg->enabled = 1u;
        int dr = dispatch_if_set(msg, tag, &cfg);
        if (dr < 0)
        {
            return ERRCODE_FAIL;
        }
        if (dr == 0)
        {
            return ERRCODE_SUCCESS;
        }
        if (isis_db_set_interface_cfg(tag, ifname, &cfg) != ERRCODE_SUCCESS)
        {
            send_resp(msg, "ISIS Error: Failed to persist interface config\r\n");
            return ERRCODE_FAIL;
        }
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (action == ISIS_IF_ACTION_METRIC)
    {
        if (is_no)
        {
            af_cfg->metric = ISIS_DEFAULT_IF_METRIC;
        }
        else if (!has_metric_val)
        {
            send_resp(msg, "ISIS Error: Missing metric value\r\n");
            return ERRCODE_FAIL;
        }
        else
        {
            af_cfg->metric = metric;
        }

        int dr = dispatch_if_set(msg, tag, &cfg);
        if (dr < 0)
        {
            return ERRCODE_FAIL;
        }
        if (dr == 0)
        {
            return ERRCODE_SUCCESS;
        }
        if (isis_db_set_interface_cfg(tag, ifname, &cfg) != ERRCODE_SUCCESS)
        {
            send_resp(msg, "ISIS Error: Failed to persist interface metric\r\n");
            return ERRCODE_FAIL;
        }
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (action == ISIS_IF_ACTION_HELLO_INTERVAL)
    {
        if (is_no)
        {
            af_cfg->hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
        }
        else if (!has_hello_val)
        {
            send_resp(msg, "ISIS Error: Missing hello-interval value\r\n");
            return ERRCODE_FAIL;
        }
        else
        {
            af_cfg->hello_interval = hello_interval;
        }

        int dr = dispatch_if_set(msg, tag, &cfg);
        if (dr < 0)
        {
            return ERRCODE_FAIL;
        }
        if (dr == 0)
        {
            return ERRCODE_SUCCESS;
        }
        if (isis_db_set_interface_cfg(tag, ifname, &cfg) != ERRCODE_SUCCESS)
        {
            send_resp(msg, "ISIS Error: Failed to persist hello-interval\r\n");
            return ERRCODE_FAIL;
        }
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (action == ISIS_IF_ACTION_HOLD_MULTIPLIER)
    {
        if (is_no)
        {
            af_cfg->hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
        }
        else if (!has_hold_val)
        {
            send_resp(msg, "ISIS Error: Missing hold-multiplier value\r\n");
            return ERRCODE_FAIL;
        }
        else
        {
            af_cfg->hold_multiplier = hold_multiplier;
        }

        int dr = dispatch_if_set(msg, tag, &cfg);
        if (dr < 0)
        {
            return ERRCODE_FAIL;
        }
        if (dr == 0)
        {
            return ERRCODE_SUCCESS;
        }
        if (isis_db_set_interface_cfg(tag, ifname, &cfg) != ERRCODE_SUCCESS)
        {
            send_resp(msg, "ISIS Error: Failed to persist hold-multiplier\r\n");
            return ERRCODE_FAIL;
        }
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (action == ISIS_IF_ACTION_PASSIVE)
    {
        af_cfg->passive = is_no ? 0u : 1u;

        int dr = dispatch_if_set(msg, tag, &cfg);
        if (dr < 0)
        {
            return ERRCODE_FAIL;
        }
        if (dr == 0)
        {
            return ERRCODE_SUCCESS;
        }
        if (isis_db_set_interface_cfg(tag, ifname, &cfg) != ERRCODE_SUCCESS)
        {
            send_resp(msg, "ISIS Error: Failed to persist passive setting\r\n");
            return ERRCODE_FAIL;
        }
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    send_resp(msg, "ISIS Error: Unknown interface command\r\n");
    return ERRCODE_FAIL;
}

int isis_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        send_resp(msg, "ISIS Error: Failed to parse command\r\n");
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    switch (parser.group_id)
    {
        case ISIS_CLI_GROUP_ID_INSTANCE:
            rc = handle_instance_cmd(msg, &parser);
            break;
        case ISIS_CLI_GROUP_ID_NET:
            rc = handle_net_cmd(msg, &parser);
            break;
        case ISIS_CLI_GROUP_ID_IS_TYPE:
            rc = handle_is_type_cmd(msg, &parser);
            break;
        case ISIS_CLI_GROUP_ID_AF:
            rc = handle_af_cmd(msg, &parser);
            break;
        case ISIS_CLI_GROUP_ID_IF_VIEW:
            rc = handle_if_view_cmd(msg, &parser);
            break;
        case ISIS_CLI_GROUP_ID_COST_STYLE:
            rc = handle_cost_style_cmd(msg, &parser);
            break;
        case ISIS_CLI_GROUP_ID_SRV6_LOCATOR:
            rc = handle_srv6_locator_cmd(msg, &parser);
            break;
        default:
            send_resp(msg, "ISIS Error: Unknown command group\r\n");
            rc = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return rc;
}
