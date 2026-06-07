/**
 * @file   lldp_worker.h
 * @brief  LLDP worker 内存态
 * @author jhb
 * @date   2026/06/07
 */
#ifndef LLDP_WORKER_H
#define LLDP_WORKER_H

#include <glib.h>
#include <pthread.h>
#include <stdint.h>

#include "dev.h"
#include "if.h"
#include "lldp_db.h"

typedef struct lldp_iface_state
{
    char ifname[IF_LOGICAL_NAME_MAX];
    uint32_t ifindex;
    uint8_t enabled;
    uint8_t link_up;
    uint8_t admin_status;
    uint8_t configured;
    uint32_t tx_interval_sec;
    uint32_t hold_multiplier;
    uint64_t last_tx_msec;
    char port_desc[LLDP_PORT_DESC_MAX];
} lldp_iface_state_t;

typedef struct lldp_neighbor
{
    char ifname[IF_LOGICAL_NAME_MAX];
    uint8_t chassis_subtype;
    uint16_t chassis_len;
    uint8_t chassis_id[256];
    uint8_t port_subtype;
    uint16_t port_len;
    uint8_t port_id[256];
    uint16_t ttl;
    uint64_t last_seen_msec;
    uint64_t expire_msec;
    char system_name[256];
    char port_desc[256];
    char system_desc[512];
    uint16_t caps_supported;
    uint16_t caps_enabled;
} lldp_neighbor_t;

typedef struct lldp_stats
{
    uint64_t tx_frames;
    uint64_t tx_errors;
    uint64_t rx_frames;
    uint64_t rx_drops;
    uint64_t rx_parse_errors;
    uint64_t neighbor_updates;
    uint64_t neighbor_deletes;
    uint64_t neighbor_expires;
} lldp_stats_t;

typedef struct lldp_work_local
{
    lldp_proto_cfg_t proto;
    GHashTable *interfaces;
    GHashTable *neighbors;
    lldp_stats_t stats;
    pthread_mutex_t lock;
    pthread_t thread;
    int running;
    int epoll_fd;
    int cmd_eventfd;
    int timer_fd;
    int raw_fd;
} lldp_work_local_t;

typedef enum lldp_apply_op
{
    LLDP_APPLY_OP_PROTO_SET = 1,
    LLDP_APPLY_OP_IF_SET = 2,
    LLDP_APPLY_OP_IF_DEL = 3,
} lldp_apply_op_t;

typedef struct lldp_apply_cmd
{
    lldp_apply_op_t op;
    int rc;
    union
    {
        lldp_proto_cfg_t proto;
        lldp_if_cfg_t if_set;
        struct
        {
            char ifname[IF_LOGICAL_NAME_MAX];
        } if_del;
    } u;
} lldp_apply_cmd_t;

extern lldp_work_local_t *g_lldp_work_local;

int lldp_worker_prepare(void);
int lldp_worker_launch(void);
void lldp_worker_shutdown(void);

int lldp_worker_post_show_cli(dev_ipc_message_t *msg);
int lldp_worker_post_if_event(dev_ipc_message_t *msg);
int lldp_worker_post_if_down(void);
int lldp_worker_dispatch_apply(lldp_apply_cmd_t *apply);

void lldp_worker_lock(void);
void lldp_worker_unlock(void);

#endif /* LLDP_WORKER_H */
