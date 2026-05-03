#include "tunnel_cli.h"

#include "tunnel_worker.h"

int tunnel_cli_handle_show(dev_ipc_message_t *msg)
{
    return tunnel_worker_post_show_cli(msg);
}

int tunnel_cli_handle_continue(dev_ipc_message_t *msg)
{
    return tunnel_worker_post_show_cli(msg);
}
