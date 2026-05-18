/**
 * @file   tunnel.h
 * @brief  Tunnel 模块公共接口：candidate、resolve watch、FTN/ILM/NHLFE 基础结构
 */
#ifndef TUNNEL_H
#define TUNNEL_H

#include <stdint.h>

#include "dev.h"
#include "net_addr.h"

#define TUNNEL_MAX_LABEL_STACK 8u
#define TUNNEL_LABEL_VALUE_MAX 0xFFFFFu

typedef enum tunnel_source_type
{
    TUNNEL_SOURCE_BGP_ADJ = 1,
    TUNNEL_SOURCE_BGP_LU = 2,
    TUNNEL_SOURCE_LDP = 3,
    TUNNEL_SOURCE_SR = 4,
    TUNNEL_SOURCE_STATIC = 5,
    TUNNEL_SOURCE_CONNECTED = 6,
} tunnel_source_type_t;

typedef enum tunnel_forward_action
{
    TUNNEL_ACTION_DROP = 0,
    TUNNEL_ACTION_PUSH = 1,
    TUNNEL_ACTION_SWAP = 2,
    TUNNEL_ACTION_POP = 3,
    TUNNEL_ACTION_PHP = 4,
} tunnel_forward_action_t;

#define TUNNEL_MSG_TYPE_CANDIDATE_ADD DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_TUNNEL, 0x0001)
#define TUNNEL_MSG_TYPE_CANDIDATE_DEL DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_TUNNEL, 0x0002)
#define TUNNEL_MSG_TYPE_RESOLVE_REGISTER DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_TUNNEL, 0x0003)
#define TUNNEL_MSG_TYPE_RESOLVE_UNREGISTER DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_TUNNEL, 0x0004)
#define TUNNEL_MSG_TYPE_RESOLVE_NOTIFY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_TUNNEL, 0x0005)
#define TUNNEL_MSG_TYPE_LABEL_ALLOC DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_TUNNEL, 0x0006)
#define TUNNEL_MSG_TYPE_LABEL_RELEASE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_TUNNEL, 0x0007)
#define TUNNEL_MSG_TYPE_RESOLVE_QUERY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_TUNNEL, 0x0008)
#define TUNNEL_MSG_TYPE_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_TUNNEL, 0x00FF)

typedef struct tunnel_fec
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t prefix_len;
    uint8_t _pad0;
    net_addr_t addr;
} tunnel_fec_t;

typedef struct tunnel_candidate
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t source_type;
    uint8_t preference;
    uint32_t owner_module_id;
    uint32_t owner_id;
    uint32_t flags;
    tunnel_fec_t fec;
    net_addr_t endpoint;
    net_addr_t nexthop;
    net_addr_t relay_addr;
    uint32_t out_ifindex;
    uint8_t label_count;
    uint8_t _pad1[3];
    uint32_t labels[TUNNEL_MAX_LABEL_STACK];
} tunnel_candidate_t;

typedef struct tunnel_resolve_req
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t _pad0[2];
    net_addr_t endpoint;
} tunnel_resolve_req_t;

typedef struct tunnel_label_req
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t source_type;
    uint8_t _pad0;
    uint32_t owner_module_id;
    uint32_t owner_id;
    uint32_t out_ifindex;
    tunnel_fec_t fec;
} tunnel_label_req_t;

typedef struct tunnel_resolve_notify
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t resolved;
    uint8_t source_type;
    uint32_t tunnel_id;
    uint32_t out_ifindex;
    net_addr_t endpoint;
    net_addr_t relay_addr;
    uint8_t label_count;
    uint8_t _pad0[3];
    uint32_t labels[TUNNEL_MAX_LABEL_STACK];
} tunnel_resolve_notify_t;

typedef struct tunnel_msg_ack
{
    int32_t result;
    uint32_t label;
} tunnel_msg_ack_t;

int tunnel_rpc_candidate_add(dev_ipc_context_t *ctx, const tunnel_candidate_t *candidate);
int tunnel_rpc_candidate_del(dev_ipc_context_t *ctx, const tunnel_candidate_t *candidate);
int tunnel_rpc_resolve_register(dev_ipc_context_t *ctx, const tunnel_resolve_req_t *req);
int tunnel_rpc_resolve_unregister(dev_ipc_context_t *ctx, const tunnel_resolve_req_t *req);
int tunnel_rpc_resolve_query(dev_ipc_context_t *ctx, const tunnel_resolve_req_t *req,
                             tunnel_resolve_notify_t *notify_out, uint32_t timeout_ms);
int tunnel_rpc_label_alloc(dev_ipc_context_t *ctx, const tunnel_label_req_t *req, uint32_t *label_out,
                           uint32_t timeout_ms);
int tunnel_rpc_label_release(dev_ipc_context_t *ctx, const tunnel_label_req_t *req);

#endif /* TUNNEL_H */
