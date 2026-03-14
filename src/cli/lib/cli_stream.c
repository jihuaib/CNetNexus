/**
 * @file   cli_stream.c
 * @brief  CLI 文本分片输出通用实现（RESP_MORE / CONTINUE）
 * @author jhb
 * @date   2026/03/14
 */

#include <string.h>

#include "cli.h"
#include "errcode.h"

static int cli_send_text_with_type(dev_ipc_context_t *ctx, uint32_t self_module_id, const dev_ipc_message_t *req,
                                   uint32_t resp_type, const char *text)
{
    if (!ctx || !req)
    {
        return ERRCODE_FAIL;
    }

    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(resp_type, self_module_id, req->src_module_id, req->request_id,
                                                     resp_data, strlen(resp_data) + 1, g_free);
    if (!resp)
    {
        g_free(resp_data);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
    return (ret == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

void cli_chunk_stream_reset(cli_chunk_stream_t *stream)
{
    if (!stream)
    {
        return;
    }
    if (stream->full_text)
    {
        g_string_free(stream->full_text, TRUE);
        stream->full_text = NULL;
    }
    stream->offset = 0;
    stream->src_module_id = 0;
}

int cli_chunk_stream_continue(cli_chunk_stream_t *stream, dev_ipc_context_t *ctx, uint32_t self_module_id,
                              const dev_ipc_message_t *req)
{
    if (!stream || !ctx || !req)
    {
        return ERRCODE_FAIL;
    }

    if (!stream->full_text || stream->src_module_id != req->src_module_id)
    {
        cli_chunk_stream_reset(stream);
        return cli_send_text_with_type(ctx, self_module_id, req, CLI_MSG_TYPE_RESP, "");
    }

    gsize full_len = stream->full_text->len;
    if (stream->offset >= full_len)
    {
        cli_chunk_stream_reset(stream);
        return cli_send_text_with_type(ctx, self_module_id, req, CLI_MSG_TYPE_RESP, "");
    }

    gsize remaining = full_len - stream->offset;
    gsize chunk_len = remaining > (CLI_MAX_RESP_LEN - 1) ? (CLI_MAX_RESP_LEN - 1) : remaining;
    char chunk[CLI_MAX_RESP_LEN];
    memcpy(chunk, stream->full_text->str + stream->offset, chunk_len);
    chunk[chunk_len] = '\0';
    stream->offset += chunk_len;

    int has_more = (stream->offset < full_len) ? 1 : 0;
    int ret =
        cli_send_text_with_type(ctx, self_module_id, req, has_more ? CLI_MSG_TYPE_RESP_MORE : CLI_MSG_TYPE_RESP, chunk);
    if (!has_more)
    {
        cli_chunk_stream_reset(stream);
    }
    return ret;
}

int cli_chunk_stream_start(cli_chunk_stream_t *stream, dev_ipc_context_t *ctx, uint32_t self_module_id,
                           const dev_ipc_message_t *req, GString *full_text)
{
    if (!stream || !ctx || !req)
    {
        if (full_text)
        {
            g_string_free(full_text, TRUE);
        }
        return ERRCODE_FAIL;
    }

    cli_chunk_stream_reset(stream);
    if (!full_text)
    {
        return cli_send_text_with_type(ctx, self_module_id, req, CLI_MSG_TYPE_RESP, "");
    }

    if (full_text->len <= (gsize)(CLI_MAX_RESP_LEN - 1))
    {
        int ret = cli_send_text_with_type(ctx, self_module_id, req, CLI_MSG_TYPE_RESP, full_text->str);
        g_string_free(full_text, TRUE);
        return ret;
    }

    stream->full_text = full_text;
    stream->offset = 0;
    stream->src_module_id = req->src_module_id;
    return cli_chunk_stream_continue(stream, ctx, self_module_id, req);
}
