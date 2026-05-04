/**
 * @file   tunnel_show.c
 * @brief  Tunnel module show command handling on worker thread.
 */
#include "tunnel_show.h"

#include <glib.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "log.h"
#include "tunnel_cli.h"
#include "tunnel_main.h"
#include "tunnel_rib.h"
#include "tunnel_worker.h"

static cli_chunk_stream_t g_tunnel_show_stream = {0};

static int tunnel_show_send_chunked(dev_ipc_message_t *msg, GString *full_text)
{
    return cli_chunk_stream_start(&g_tunnel_show_stream, tunnel_local_ipc_ctx(), DEV_MODULE_ID_TUNNEL, msg, full_text);
}

static int tunnel_show_handle_tunnel(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int show_label = 0;
    tunnel_show_section_t section = TUNNEL_SHOW_SUMMARY;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1:
                show_label = 1;
                break;
            case 2:
                section = TUNNEL_SHOW_CANDIDATE;
                break;
            case 3:
                section = TUNNEL_SHOW_NHLFE;
                break;
            case 4:
                section = TUNNEL_SHOW_FTN;
                break;
            case 5:
                section = TUNNEL_SHOW_ILM;
                break;
            case 6:
                section = TUNNEL_SHOW_WATCH;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    char *text = show_label ? tunnel_rib_show_labels(g_tunnel_work_local ? g_tunnel_work_local->rib : NULL)
                            : tunnel_rib_show(g_tunnel_work_local ? g_tunnel_work_local->rib : NULL, section);
    GString *out = text ? g_string_new(text) : NULL;
    g_free(text);
    return tunnel_show_send_chunked(msg, out);
}

int tunnel_show_dispatch(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    if (msg->msg_type == CLI_MSG_TYPE_CONTINUE)
    {
        return cli_chunk_stream_continue(&g_tunnel_show_stream, tunnel_local_ipc_ctx(), DEV_MODULE_ID_TUNNEL, msg);
    }

    if (!msg->payload)
    {
        return ERRCODE_FAIL;
    }

    tunnel_show_cleanup_state();

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("TUNNEL: show payload parse failed");
        return ERRCODE_FAIL;
    }

    int result;
    switch (parser.group_id)
    {
        case TUNNEL_CLI_GROUP_ID_SHOW:
            result = tunnel_show_handle_tunnel(msg, &parser);
            break;
        default:
            LOG_WARN("TUNNEL: unknown show group_id=%u", parser.group_id);
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}

void tunnel_show_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_tunnel_show_stream);
}
