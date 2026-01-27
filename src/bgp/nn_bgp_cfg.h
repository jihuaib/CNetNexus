#ifndef NN_BGP_CFG_H
#define NN_BGP_CFG_H

#include <stdbool.h>

#include "nn_dev.h"

// ============================================================================
// Command Group and Element IDs
// ============================================================================

#define NN_BGP_CLI_GROUP_ID_BGP 0x00000001
#define NN_BGP_CLI_BGP_CFG_ID_BGP_NO 0x00000001
#define NN_BGP_CLI_BGP_CFG_ID_BGP_AS 0x00000002

// ============================================================================
// Configuration Data Types
// ============================================================================

/**
 * @brief BGP instance configuration data
 */
typedef struct nn_bgp_cfg_data_bgp
{
    bool no;
    uint32_t as_number;
} nn_bgp_cfg_data_bgp_t;

/**
 * @brief BGP peer configuration data
 */
typedef struct nn_bgp_cfg_data_peer
{
    char peer_ip[64];
    uint32_t peer_as;
} nn_bgp_cfg_data_peer_t;

// ============================================================================
// Configuration Output Structure (for view switching and prompt)
// ============================================================================

/**
 * @brief Configuration output from command handler
 * Used for view switching and prompt generation
 */
typedef struct nn_bgp_cfg_out
{
    uint32_t group_id;
    union
    {
        nn_bgp_cfg_data_bgp_t bgp;
        nn_bgp_cfg_data_peer_t peer;
    } data;
} nn_bgp_cfg_out_t;

// ============================================================================
// Response Output Structure (for display messages)
// ============================================================================

/**
 * @brief Response output from command handler
 * Used for displaying messages to user
 */
typedef struct nn_bgp_resp_out
{
    char message[256]; // Response message to display
    int success;       // Command execution result
} nn_bgp_resp_out_t;

int nn_bgp_cfg_handle_message(nn_dev_message_t *msg);

#endif // NN_BGP_CFG_H
