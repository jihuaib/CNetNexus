/**
 * @file   fib.h
 * @brief  FIB 模块公共接口：route/tunnel join 后统一下发 OS FIB
 */
#ifndef FIB_H
#define FIB_H

#include <stdint.h>

#include "dev.h"
#include "net_addr.h"

#define FIB_MAX_LABEL_STACK 8u

#define FIB_ROUTE_FLAG_SKIP_OS (1u << 0)

#define FIB_MSG_TYPE_ROUTE_UPSERT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0001)
#define FIB_MSG_TYPE_ROUTE_DELETE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0002)
#define FIB_MSG_TYPE_TUNNEL_UPSERT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0003)
#define FIB_MSG_TYPE_TUNNEL_DELETE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0004)
#define FIB_MSG_TYPE_ROUTE_RESULT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0005)
#define FIB_MSG_TYPE_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x00FF)

typedef enum fib_nh_type
{
    FIB_NH_TYPE_IP = 1,
    FIB_NH_TYPE_TUNNEL = 2,
    FIB_NH_TYPE_BLACKHOLE = 3,
} fib_nh_type_t;

typedef struct fib_route_entry
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t safi;
    uint8_t prefix_len;
    uint32_t protocol; /**< ROUTE_PROTOCOL_* */
    int32_t metric;
    int32_t preference;
    uint32_t flags;
    uint8_t nh_type;
    uint8_t _pad0[3];
    uint32_t tunnel_id;
    uint32_t out_ifindex;
    net_addr_t prefix_addr;
    net_addr_t nexthop_addr;
} fib_route_entry_t;

typedef struct fib_tunnel_entry
{
    uint32_t tunnel_id;
    uint8_t state;
    uint8_t label_count;
    uint8_t _pad0[2];
    uint32_t out_ifindex;
    net_addr_t relay_addr;
    uint32_t labels[FIB_MAX_LABEL_STACK];
} fib_tunnel_entry_t;

typedef struct fib_msg_ack
{
    int32_t result;
} fib_msg_ack_t;

typedef struct fib_route_result
{
    fib_route_entry_t entry;
    int32_t result;
    uint32_t op_msg_type;
} fib_route_result_t;

int fib_rpc_route_upsert(dev_ipc_context_t *ctx, const fib_route_entry_t *entry);
int fib_rpc_route_delete(dev_ipc_context_t *ctx, const fib_route_entry_t *entry);
int fib_rpc_tunnel_upsert(dev_ipc_context_t *ctx, const fib_tunnel_entry_t *entry);
int fib_rpc_tunnel_delete(dev_ipc_context_t *ctx, const fib_tunnel_entry_t *entry);

#endif /* FIB_H */
