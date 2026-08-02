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
#define FIB_ROUTE_FLAG_LOCAL (1u << 1)

#define FIB_MSG_TYPE_ROUTE_UPSERT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0001)
#define FIB_MSG_TYPE_ROUTE_DELETE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0002)
#define FIB_MSG_TYPE_TUNNEL_UPSERT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0003)
#define FIB_MSG_TYPE_TUNNEL_DELETE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0004)
#define FIB_MSG_TYPE_ROUTE_RESULT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0005)
#define FIB_MSG_TYPE_ILM_UPSERT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0006)
#define FIB_MSG_TYPE_ILM_DELETE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0007)
#define FIB_MSG_TYPE_NEXTHOP_UPSERT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0008)
#define FIB_MSG_TYPE_NEXTHOP_DELETE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x0009)
#define FIB_MSG_TYPE_SRV6_LOCALSID_UPSERT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x000A)
#define FIB_MSG_TYPE_SRV6_LOCALSID_DELETE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x000B)
#define FIB_MSG_TYPE_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_FIB, 0x00FF)

typedef enum fib_nh_type
{
    FIB_NH_TYPE_IP = 1,
    FIB_NH_TYPE_TUNNEL = 2,
    FIB_NH_TYPE_BLACKHOLE = 3,
    /** SRv6 BE：向 srv6_sid 做单段 IPv6 encapsulation。 */
    FIB_NH_TYPE_SRV6 = 4,
} fib_nh_type_t;

/** RFC 8986/IANA SRv6 Endpoint Behavior codepoints used on the IPC boundary. */
typedef enum fib_srv6_behavior
{
    FIB_SRV6_BEHAVIOR_END_DT6 = 18,
    FIB_SRV6_BEHAVIOR_END_DT4 = 19,
} fib_srv6_behavior_t;

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
    uint32_t out_label; /**< 隧道转发时压入的出标签（如 L3VPN 私网/VPN 标签），0=无需压标签 */
    uint32_t nexthop_id; /**< nh_type=IP/BLACKHOLE 时引用的 nexthop 对象 ID（由 ROUTE 分配，0=未对象化） */
    uint32_t out_ifindex;
    net_addr_t prefix_addr;
    net_addr_t nexthop_addr;
    /** nh_type=FIB_NH_TYPE_SRV6 时的远端 L3 service SID。 */
    net_addr_t srv6_sid;
} fib_route_entry_t;

/** SRv6 local SID owned by the SRV6 module and programmed by FIB. */
typedef struct fib_srv6_localsid_entry
{
    uint32_t vrf_id;
    uint16_t behavior;  /**< FIB_SRV6_BEHAVIOR_END_DT4/END_DT6 */
    uint8_t prefix_len; /**< basic service SID is installed as /128 */
    uint8_t flags;
    net_addr_t sid;
} fib_srv6_localsid_entry_t;

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

typedef struct fib_nexthop_entry
{
    uint32_t nexthop_id;
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t nh_type; /**< FIB_NH_TYPE_IP / FIB_NH_TYPE_BLACKHOLE */
    uint8_t state;   /**< 1=就绪 */
    uint32_t out_ifindex;
    net_addr_t gateway_addr;
} fib_nexthop_entry_t;

typedef struct fib_ilm_entry
{
    uint32_t vrf_id;
    uint32_t in_label;
    uint8_t action; /**< TUNNEL_ACTION_* */
    uint8_t state;
    uint8_t label_count;
    uint8_t _pad0;
    uint32_t nhlfe_id;
    uint32_t out_ifindex;
    net_addr_t relay_addr;
    uint32_t labels[FIB_MAX_LABEL_STACK];
} fib_ilm_entry_t;

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
int fib_rpc_ilm_upsert(dev_ipc_context_t *ctx, const fib_ilm_entry_t *entry);
int fib_rpc_ilm_delete(dev_ipc_context_t *ctx, const fib_ilm_entry_t *entry);
int fib_rpc_nexthop_upsert(dev_ipc_context_t *ctx, const fib_nexthop_entry_t *entry);
int fib_rpc_nexthop_delete(dev_ipc_context_t *ctx, const fib_nexthop_entry_t *entry);
int fib_rpc_srv6_localsid_upsert(dev_ipc_context_t *ctx, const fib_srv6_localsid_entry_t *entry);
int fib_rpc_srv6_localsid_delete(dev_ipc_context_t *ctx, const fib_srv6_localsid_entry_t *entry);
int fib_rpc_srv6_localsid_upsert_wait(dev_ipc_context_t *ctx, const fib_srv6_localsid_entry_t *entry,
                                      uint32_t timeout_ms);
int fib_rpc_srv6_localsid_delete_wait(dev_ipc_context_t *ctx, const fib_srv6_localsid_entry_t *entry,
                                      uint32_t timeout_ms);

#endif /* FIB_H */
