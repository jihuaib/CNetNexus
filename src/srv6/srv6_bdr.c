#include "srv6_bdr.h"

#include <netinet/in.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "srv6_cli.h"
#include "srv6_db.h"

static gint srv6_bdr_locator_sort(gconstpointer a, gconstpointer b)
{
    const srv6_locator_t *const *la = a;
    const srv6_locator_t *const *lb = b;
    return strcmp((*la)->name, (*lb)->name);
}

int srv6_bdr_handle_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse(msg->payload, msg->payload_len, &scope) != 0)
    {
        srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP_ERROR, "SRV6 BDR: invalid show scope");
        return ERRCODE_FAIL;
    }
    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS && strcmp(scope.view_name, "srv6") != 0)
    {
        srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP, "");
        return ERRCODE_SUCCESS;
    }

    GPtrArray *locators = NULL;
    if (srv6_db_locator_list(&locators) != ERRCODE_SUCCESS)
    {
        srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP_ERROR, "SRV6 BDR: locator query failed");
        return ERRCODE_FAIL;
    }
    g_ptr_array_sort(locators, srv6_bdr_locator_sort);
    GString *out = g_string_new(NULL);
    if (locators->len > 0u)
    {
        g_string_append(out, "!\r\nsrv6\r\n");
        for (guint i = 0; i < locators->len; ++i)
        {
            const srv6_locator_t *locator = g_ptr_array_index(locators, i);
            char prefix[INET6_ADDRSTRLEN];
            net_addr_to_str(&locator->prefix, prefix, sizeof(prefix));
            g_string_append_printf(out, " locator %s prefix %s %u function-bits %u\r\n", locator->name, prefix,
                                   (unsigned)locator->prefix_len, (unsigned)locator->function_bits);
        }
        g_string_append(out, "!\r\n");
    }
    srv6_cli_send_response(msg, CLI_MSG_TYPE_RESP, out->str);
    g_string_free(out, TRUE);
    g_ptr_array_free(locators, TRUE);
    return ERRCODE_SUCCESS;
}
