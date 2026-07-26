/**
 * @file   ospfv3_worker.h
 * @brief  OSPFv3 worker state and command interface
 */
#ifndef OSPFV3_WORKER_H
#define OSPFV3_WORKER_H

#include <glib.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>

#include "dev.h"
#include "if.h"
#include "net_addr.h"
#include "ospfv3.h"

typedef enum ospfv3_interface_state
{
    OSPFV3_IF_STATE_DOWN = 0,
    OSPFV3_IF_STATE_WAITING = 1,
    OSPFV3_IF_STATE_POINT_TO_POINT = 2,
    OSPFV3_IF_STATE_DR_OTHER = 3,
    OSPFV3_IF_STATE_BACKUP = 4,
    OSPFV3_IF_STATE_DR = 5,
} ospfv3_interface_state_t;

typedef enum ospfv3_neighbor_state
{
    OSPFV3_NBR_STATE_DOWN = 0,
    OSPFV3_NBR_STATE_INIT = 1,
    OSPFV3_NBR_STATE_TWO_WAY = 2,
    OSPFV3_NBR_STATE_EXSTART = 3,
    OSPFV3_NBR_STATE_EXCHANGE = 4,
    OSPFV3_NBR_STATE_LOADING = 5,
    OSPFV3_NBR_STATE_FULL = 6,
} ospfv3_neighbor_state_t;

typedef struct ospfv3_if_cfg
{
    char ifname[IF_LOGICAL_NAME_MAX];
    char vrf_name[IF_VRF_NAME_MAX];
    uint8_t enabled;
    uint8_t passive;
    uint8_t network_type;
    uint8_t priority;
    uint16_t cost;
    uint16_t hello_interval;
    uint32_t dead_interval;
    uint32_t area_id;
    uint32_t dr;
    uint32_t bdr;
    uint8_t state;
    uint8_t joined_all_routers;
    uint8_t joined_dr_routers;
    uint8_t _pad0;
    uint64_t last_hello_tx_msec;
    uint64_t wait_until_msec;
} ospfv3_if_cfg_t;

typedef struct ospfv3_neighbor
{
    char ifname[IF_LOGICAL_NAME_MAX];
    uint32_t area_id;
    uint32_t router_id;
    struct in6_addr src_addr;
    uint32_t interface_id;
    uint32_t dr;
    uint32_t bdr;
    uint32_t dd_sequence;
    uint32_t dead_interval;
    uint32_t dd_summary_cursor;
    uint16_t dd_peer_mtu;
    uint8_t priority;
    uint32_t options;
    uint8_t state;
    uint8_t dd_master;
    uint8_t dd_peer_more;
    uint8_t dd_local_more;
    uint32_t dd_options;
    uint8_t _pad0[7];
    uint64_t last_seen_msec;
    uint64_t last_dbd_tx_msec;
    uint64_t last_lsa_retransmit_msec;
    GPtrArray *request_keys;
    GPtrArray *dd_summaries;
    GByteArray *last_dbd_tx;
    GByteArray *last_dbd_rx;
    GHashTable *retrans_lsas;
} ospfv3_neighbor_t;

typedef struct ospfv3_lsa_entry
{
    uint32_t area_id;
    uint32_t link_state_id;
    uint32_t advertising_router;
    uint32_t sequence;
    uint16_t age;
    uint16_t checksum;
    uint16_t length;
    uint16_t type;
    uint32_t options;
    uint8_t self_originated;
    uint8_t _pad0;
    uint64_t installed_msec;
    GByteArray *raw;
} ospfv3_lsa_entry_t;

typedef struct ospfv3_route
{
    net_addr_t prefix;
    net_addr_t source;
    net_addr_t nexthop;
    uint32_t out_ifindex;
    uint32_t metric;
    uint32_t advertising_router;
    uint32_t vrf_id;
    uint8_t prefix_len;
    uint8_t _pad0[3];
} ospfv3_route_t;

typedef struct ospfv3_instance
{
    uint32_t process_id;
    uint32_t router_id;
    uint32_t vrf_id;
    char vrf_name[IF_VRF_NAME_MAX];
    uint8_t admin_up;
    uint8_t _pad0[3];
    GHashTable *areas;
    GHashTable *if_cfgs;
    GHashTable *neighbors;
    GHashTable *lsdb;
    GHashTable *routes;
    uint32_t next_lsa_sequence;
    uint64_t last_lsa_refresh_msec;
    uint64_t next_lsa_originate_msec;
} ospfv3_instance_t;

typedef enum ospfv3_apply_op
{
    OSPFV3_APPLY_INSTANCE_SET = 1,
    OSPFV3_APPLY_INSTANCE_DEL = 2,
    OSPFV3_APPLY_ROUTER_ID_SET = 3,
    OSPFV3_APPLY_IF_SET = 4,
    OSPFV3_APPLY_IF_DEL = 5,
    OSPFV3_APPLY_AREA_SET = 6,
    OSPFV3_APPLY_AREA_DEL = 7,
} ospfv3_apply_op_t;

typedef enum ospfv3_apply_rc
{
    OSPFV3_APPLY_RC_OK = 0,
    OSPFV3_APPLY_RC_NOOP = 1,
    OSPFV3_APPLY_RC_FAIL = -1,
} ospfv3_apply_rc_t;

typedef struct ospfv3_apply_cmd
{
    ospfv3_apply_op_t op;
    ospfv3_apply_rc_t rc;
    char errmsg[256];
    union
    {
        struct
        {
            uint32_t process_id;
            uint32_t router_id;
            uint32_t vrf_id;
            char vrf_name[IF_VRF_NAME_MAX];
        } instance_set;
        struct
        {
            uint32_t process_id;
        } instance_del;
        struct
        {
            uint32_t process_id;
            uint32_t router_id;
        } router_id_set;
        struct
        {
            uint32_t process_id;
            ospfv3_if_cfg_t cfg;
        } if_set;
        struct
        {
            uint32_t process_id;
            char ifname[IF_LOGICAL_NAME_MAX];
        } if_del;
        struct
        {
            uint32_t process_id;
            uint32_t area_id;
        } area_set;
        struct
        {
            uint32_t process_id;
            uint32_t area_id;
        } area_del;
    } u;
} ospfv3_apply_cmd_t;

typedef struct ospfv3_work_local
{
    GHashTable *instances;
    int epoll_fd;
    int cmd_eventfd;
    int timer_fd;
    int raw_fd;
    pthread_t thread;
    volatile int running;
    GAsyncQueue *cmd_queue;
} ospfv3_work_local_t;

extern ospfv3_work_local_t *g_ospfv3_work_local;

uint64_t ospfv3_now_msec(void);
gboolean ospfv3_if_entry_matches_vrf(const char *vrf_name, const if_api_cache_entry_t *entry);
ospfv3_instance_t *ospfv3_lookup_instance(uint32_t process_id);
ospfv3_instance_t *ospfv3_get_or_create_instance(uint32_t process_id);
gboolean ospfv3_instance_has_area(const ospfv3_instance_t *inst, uint32_t area_id);

int ospfv3_worker_prepare(void);
int ospfv3_worker_launch(void);
void ospfv3_worker_shutdown(void);
int ospfv3_worker_post_show_cli(dev_ipc_message_t *msg);
int ospfv3_worker_post_if_event(dev_ipc_message_t *msg);
int ospfv3_worker_post_route_ready(void);
int ospfv3_worker_post_if_down(void);
int ospfv3_worker_dispatch_apply(ospfv3_apply_cmd_t *apply);

void ospfv3_worker_reconcile_all(void);
void ospfv3_worker_replay_routes(void);
void ospfv3_worker_withdraw_all_routes(ospfv3_instance_t *inst);

#endif /* OSPFV3_WORKER_H */
