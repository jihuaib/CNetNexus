#ifndef NN_IF_CLI_H
#define NN_IF_CLI_H

#include <glib.h>

#include "nn_cfg.h"
#include "nn_dev.h"

// ============================================================================
// Command Group and Element IDs
// ============================================================================

#define NN_IF_CLI_GROUP_ID_INTERFACE 1
#define NN_IF_CLI_IF_CFG_ID_IFNAME 1 // Still valid for generic strings if used elsewhere
#define NN_IF_CLI_IF_CFG_ID_GE1 1
#define NN_IF_CLI_IF_CFG_ID_GE2 2
#define NN_IF_CLI_IF_CFG_ID_GE3 3
#define NN_IF_CLI_IF_CFG_ID_GE4 4

#define NN_IF_CLI_GROUP_ID_CONFIG 2
#define NN_IF_CLI_IF_CFG_ID_IP 1
#define NN_IF_CLI_IF_CFG_ID_MASK 2
#define NN_IF_CLI_IF_CFG_ID_SHUTDOWN 3
#define NN_IF_CLI_IF_CFG_ID_UNDO 4

#define NN_IF_CLI_GROUP_ID_SHOW 3
#define NN_IF_CLI_IF_CFG_ID_SHOW_NAME 1

// ============================================================================
// Configuration Data Types
// ============================================================================

typedef struct nn_if_cli_data_interface
{
    char ifname[32];
} nn_if_cli_data_interface_t;

typedef struct nn_if_cli_data_config
{
    gboolean undo;
    gboolean has_ip;
    char ip[20];
    char mask[20];
    gboolean shutdown;
} nn_if_cli_data_config_t;

typedef struct nn_if_cli_data_show
{
    gboolean has_ifname;
    char ifname[32];
} nn_if_cli_data_show_t;

// ============================================================================
// Configuration Output Structure
// ============================================================================

typedef struct nn_if_cli_out
{
    uint32_t group_id;
    union
    {
        nn_if_cli_data_interface_t interface;
        nn_if_cli_data_config_t config;
        nn_if_cli_data_show_t show;
    } data;
} nn_if_cli_out_t;

// ============================================================================
// Response Output Structure
// ============================================================================

typedef struct nn_if_cli_resp_out
{
    char message[NN_CFG_CLI_MAX_RESP_LEN];
    int success;
} nn_if_cli_resp_out_t;

int nn_if_cli_handle_message(nn_dev_message_t *msg);

#endif // NN_IF_CLI_H
