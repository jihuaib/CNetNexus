/**
 * @file   isis_worker.h
 * @brief  ISIS worker 线程：配置内存态、show 输出与 IF 事件消费
 * @author jhb
 * @date   2026/04/11
 */
#ifndef ISIS_WORKER_H
#define ISIS_WORKER_H

#include <glib.h>
#include <pthread.h>
#include <stdint.h>

#include "dev.h"
#include "if_event.h"
#include "isis.h"

#define ISIS_NET_STR_MAX 64

typedef struct isis_if_af_cfg
{
    uint8_t enabled;
    uint8_t passive;
    uint16_t hello_interval;
    uint8_t hold_multiplier;
    uint32_t metric;
} isis_if_af_cfg_t;

typedef struct isis_if_cfg
{
    char ifname[IF_LOGICAL_NAME_MAX];
    isis_if_af_cfg_t v4;
    isis_if_af_cfg_t v6;
    uint64_t last_hello_tx_msec; /**< interface-level IIH tx pacing timestamp */
} isis_if_cfg_t;

static inline isis_if_af_cfg_t *isis_if_cfg_af(isis_if_cfg_t *cfg, uint16_t afi)
{
    if (!cfg)
    {
        return NULL;
    }
    if (afi == ISIS_AFI_IPV4)
    {
        return &cfg->v4;
    }
    if (afi == ISIS_AFI_IPV6)
    {
        return &cfg->v6;
    }
    return NULL;
}

static inline const isis_if_af_cfg_t *isis_if_cfg_af_const(const isis_if_cfg_t *cfg, uint16_t afi)
{
    if (!cfg)
    {
        return NULL;
    }
    if (afi == ISIS_AFI_IPV4)
    {
        return &cfg->v4;
    }
    if (afi == ISIS_AFI_IPV6)
    {
        return &cfg->v6;
    }
    return NULL;
}

static inline int isis_if_cfg_any_enabled(const isis_if_cfg_t *cfg)
{
    return (cfg && (cfg->v4.enabled || cfg->v6.enabled)) ? 1 : 0;
}

typedef struct isis_route_state
{
    uint16_t afi;
    uint8_t prefix_len;
    uint8_t _pad0;
    net_addr_t prefix_addr;
    net_addr_t source_addr;
    net_addr_t nexthop_addr;
    uint32_t out_ifindex;
    uint32_t metric;
} isis_route_state_t;

typedef enum isis_adj_state
{
    ISIS_ADJ_STATE_DOWN = 0,
    ISIS_ADJ_STATE_INIT = 1,
    ISIS_ADJ_STATE_UP = 2,
} isis_adj_state_t;

typedef struct isis_neighbor
{
    char ifname[IF_LOGICAL_NAME_MAX];
    uint8_t system_id[6];
    uint8_t level;
    uint8_t state;
    uint8_t priority;
    uint16_t hold_time_sec;
    net_addr_t ipv4_addr;
    net_addr_t ipv6_addr;
    uint32_t last_lsp_seq;
    uint64_t last_lsp_rx_msec;
    uint64_t last_seen_msec;
} isis_neighbor_t;

typedef struct isis_lsdb_entry
{
    char rx_ifname[IF_LOGICAL_NAME_MAX];
    uint8_t system_id[6];
    uint8_t level;
    uint8_t _pad0;
    uint16_t lifetime_sec;
    uint16_t checksum;
    uint32_t seq;
    uint32_t ipv4_prefix_count;
    uint32_t ipv6_prefix_count;
    uint64_t last_rx_msec;
    GByteArray *tlvs; /**< latest raw TLVs from received LSP */
} isis_lsdb_entry_t;

typedef struct isis_instance_cfg
{
    uint32_t tag;
    char net[ISIS_NET_STR_MAX];
    uint8_t is_type;
    uint8_t admin_up;
    uint8_t af_ipv4;
    uint8_t af_ipv6;
    GHashTable *if_cfgs;        /**< key=ifname(strdup), value=isis_if_cfg_t* */
    GHashTable *route_states;   /**< key=ifname|afi(strdup), value=isis_route_state_t* */
    GHashTable *learned_routes; /**< key=learned-route-id(strdup), value=isis_route_state_t* */
    GHashTable *neighbors;      /**< key=ifname|level|sysid(strdup), value=isis_neighbor_t* */
    GHashTable *lsdb_entries;   /**< key=level|sysid(strdup), value=isis_lsdb_entry_t* */
    uint32_t lsp_seq_l1;
    uint32_t lsp_seq_l2;
    uint64_t last_lsp_tx_msec;
} isis_instance_cfg_t;

typedef enum isis_apply_op
{
    ISIS_APPLY_OP_INSTANCE_SET = 1,
    ISIS_APPLY_OP_INSTANCE_DEL = 2,
    ISIS_APPLY_OP_NET_SET = 3,
    ISIS_APPLY_OP_IS_TYPE_SET = 4,
    ISIS_APPLY_OP_AF_SET = 5,
    ISIS_APPLY_OP_AF_DEL = 6,
    ISIS_APPLY_OP_IF_SET = 7,
    ISIS_APPLY_OP_IF_DEL = 8,
} isis_apply_op_t;

typedef struct isis_apply_cmd
{
    isis_apply_op_t op;
    int rc;

    union
    {
        struct
        {
            uint32_t tag;
            char net[ISIS_NET_STR_MAX];
            uint8_t is_type;
            uint8_t admin_up;
        } instance_set;
        struct
        {
            uint32_t tag;
        } instance_del;
        struct
        {
            uint32_t tag;
            char net[ISIS_NET_STR_MAX];
        } net_set;
        struct
        {
            uint32_t tag;
            uint8_t is_type;
        } is_type_set;
        struct
        {
            uint32_t tag;
            uint16_t afi;
        } af_set;
        struct
        {
            uint32_t tag;
            uint16_t afi;
        } af_del;
        struct
        {
            uint32_t tag;
            isis_if_cfg_t cfg;
        } if_set;
        struct
        {
            uint32_t tag;
            char ifname[IF_LOGICAL_NAME_MAX];
        } if_del;
    } u;
} isis_apply_cmd_t;

typedef struct isis_work_local
{
    GHashTable *instances; /**< key=GUINT_TO_POINTER(tag), value=isis_instance_cfg_t* */

    int epoll_fd;
    int cmd_eventfd;
    pthread_t thread;
    volatile int running;
    GAsyncQueue *cmd_queue;
} isis_work_local_t;

extern isis_work_local_t *g_isis_work_local;

int isis_worker_prepare(void);
int isis_worker_launch(void);
void isis_worker_shutdown(void);

int isis_worker_post_show_cli(dev_ipc_message_t *msg);
int isis_worker_post_if_event(dev_ipc_message_t *msg);

int isis_worker_dispatch_apply(isis_apply_cmd_t *apply);

#endif /* ISIS_WORKER_H */
