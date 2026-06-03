#include <glib.h>
#include <stdint.h>
#include <string.h>

#include "access.h"
#include "cli.h"
#include "errcode.h"

int cli_line_progress_send(dev_ipc_context_t *ctx, uint32_t line_id, const char *text)
{
    if (!ctx || line_id == UINT32_MAX || !text || text[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    if (!dev_ipc_is_connected(ctx, DEV_MODULE_ID_ACCESS))
    {
        (void)dev_ipc_connect(ctx, DEV_MODULE_ID_ACCESS, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_ACCESS);
        if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_ACCESS, 500) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
    }

    size_t text_len = strlen(text);
    size_t payload_len = sizeof(access_line_progress_t) + text_len + 1;
    access_line_progress_t *payload = g_malloc0(payload_len);
    payload->line_id = line_id;
    memcpy(payload->text, text, text_len + 1);

    dev_ipc_message_t *msg = dev_ipc_message_create(ACCESS_MSG_LINE_PROGRESS, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_ACCESS, 0, payload, payload_len, g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int rc = dev_ipc_send(ctx, DEV_MODULE_ID_ACCESS, msg);
    dev_ipc_message_free(msg);
    return (rc == ERRCODE_SUCCESS) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}
