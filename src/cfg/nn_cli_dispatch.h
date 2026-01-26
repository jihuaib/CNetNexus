//
// Created by jhb on 1/25/26.
// CLI command dispatch - TLV message packing and module dispatch
//

#ifndef NN_CLI_DISPATCH_H
#define NN_CLI_DISPATCH_H

#include <stdint.h>

#include "nn_cli_tree.h"

// TLV message format for command dispatch:
// +----------------+
// | group_id (4B)  |  - Target group ID
// +----------------+
// | TLV element 1  |  - First matched element
// +----------------+
// | TLV element 2  |  - Second matched element
// +----------------+
// | ...            |
// +----------------+
//
// TLV element format:
// +----------------+
// | element_id(4B) |  - Element ID (type)
// +----------------+
// | length (2B)    |  - Value length (0 for keywords)
// +----------------+
// | value (N bytes)|  - Value data (only for arguments)
// +----------------+

#include "nn_cli_handler.h"

// Pack match result into TLV buffer
// Returns allocated buffer, caller must free with g_free()
// out_len receives the total buffer length
uint8_t *nn_cli_dispatch_pack_tlv(nn_cli_match_result_t *result, uint32_t *out_len);

// Dispatch command to target module via pub/sub
// Returns 0 on success, -1 on failure
int nn_cli_dispatch_to_module(nn_cli_match_result_t *result, nn_cli_session_t *session);

#endif // NN_CLI_DISPATCH_H
