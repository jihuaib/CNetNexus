/**
 * @file   nn_bgp_cli.h
 * @brief  BGP 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef NN_BGP_CLI_H
#define NN_BGP_CLI_H

#include "nn_cfg.h"
#include "nn_dev.h"

// ============================================================================
// Command Group and Element IDs
// ============================================================================

#define NN_BGP_CLI_GROUP_ID_BGP 0x00000001
#define NN_BGP_CLI_BGP_CFG_ID_BGP_NO 0x00000001
#define NN_BGP_CLI_BGP_CFG_ID_BGP_AS 0x00000002

#define NN_BGP_CLI_GROUP_ID_SHOW 0x00000002
#define NN_BGP_CLI_SHOW_CFG_ID_PEER 0x00000001

// ============================================================================
// Configuration Data Types
// ============================================================================

/**
 * @brief BGP instance configuration data
 */
typedef struct nn_bgp_cli_data_bgp
{
    gboolean no;
    uint32_t as_number;
} nn_bgp_cli_data_bgp_t;

/**
 * @brief BGP peer configuration data
 */
typedef struct nn_bgp_cli_data_peer
{
    char peer_ip[64];
    uint32_t peer_as;
} nn_bgp_cli_data_peer_t;

// ============================================================================
// Configuration Output Structure (for view switching and prompt)
// ============================================================================

/**
 * @brief Configuration output from command handler
 * Used for view switching and prompt generation
 */
typedef struct nn_bgp_cli_out
{
    uint32_t group_id;
    union
    {
        nn_bgp_cli_data_bgp_t bgp;
        nn_bgp_cli_data_peer_t peer;
    } data;
} nn_bgp_cli_out_t;

// ============================================================================
// Response Output Structure (for display messages)
// ============================================================================

/**
 * @brief Response output from command handler
 * Used for displaying messages to user
 */
typedef struct nn_bgp_cli_resp_out
{
    char message[NN_CFG_CLI_MAX_RESP_LEN]; // Response message to display
    int success;                           // Command execution result
    uint32_t has_more;                     // 1 if more data available
    uint32_t batch_offset;                 // Continuation offset for next batch
} nn_bgp_cli_resp_out_t;

int nn_bgp_cli_handle_message(nn_dev_message_t *msg);
int nn_bgp_cli_handle_continue(nn_dev_message_t *msg);

#endif // NN_BGP_CLI_H
