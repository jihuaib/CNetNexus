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
#include "if.h"
#include "isis.h"

#define ISIS_NET_STR_MAX 64

typedef struct isis_nexthop_table isis_nexthop_table_t;

typedef struct isis_if_af_cfg
{
    uint8_t enabled;
    uint8_t passive;
    uint16_t hello_interval;
    uint8_t hold_multiplier;
    uint32_t metric;
} isis_if_af_cfg_t;

/** 单 (interface, level) 的 DIS 选举状态 */
typedef struct isis_dis_state
{
    uint8_t lan_id[7];      /**< 当前 DIS 的 LAN-ID（sysid 6B + circuit-id 1B） */
    uint8_t we_are_dis;     /**< 我们是当前 DIS？ */
    uint8_t our_circuit_id; /**< 我们做 DIS 时使用的 circuit-id（非 0） */
    uint8_t _pad0;
    uint64_t last_election_msec; /**< 最近一次跑选举的时间，用于节流 */
    uint32_t pseudo_seq;         /**< 我们做 DIS 时的伪节点 LSP 序列号 */
} isis_dis_state_t;

typedef struct isis_if_cfg
{
    char ifname[IF_LOGICAL_NAME_MAX];
    isis_if_af_cfg_t v4;
    isis_if_af_cfg_t v6;
    uint64_t last_hello_tx_msec; /**< interface-level IIH tx pacing timestamp */
    isis_dis_state_t dis_l1;     /**< 该接口 L1 的 DIS 状态 */
    isis_dis_state_t dis_l2;     /**< 该接口 L2 的 DIS 状态 */
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
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t prefix_len;
    uint8_t _pad0;
    net_addr_t prefix_addr;
    net_addr_t source_addr;
    uint32_t metric;
    uint32_t nexthop_id;
    isis_nexthop_table_t *nexthop_table; /**< 持有 nexthop_id 的地址族表（借用，随 instance 生命周期） */
} isis_route_state_t;

typedef struct isis_route_head isis_route_head_t;

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
    uint8_t local_snpa[6];
    uint8_t remote_snpa[6];
    uint8_t remote_circuit_type;
    uint8_t remote_ipv4_nlpid;
    uint8_t remote_ipv6_nlpid;
    uint8_t seen_self;
    uint8_t area_match;
    uint8_t circuit_ok;
    uint8_t nlpids_ok;
    uint8_t hold_ok;
    uint8_t hello_valid;
    uint8_t _pad0[3];
    uint8_t remote_lan_id[7]; /**< 邻居在 IIH 里声明的 LAN-ID（DIS sysid + circuit-id） */
    uint8_t _pad1;
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
    uint8_t pseudonode_id;
    uint8_t fragment_id;
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
    uint32_t vrf_id;
    char vrf_name[IF_VRF_NAME_MAX];
    char net[ISIS_NET_STR_MAX];
    uint8_t is_type;
    uint8_t admin_up;
    uint8_t af_ipv4;
    uint8_t af_ipv6;
    uint8_t cost_style;               /**< ISIS_COST_STYLE_NARROW / ISIS_COST_STYLE_WIDE */
    GHashTable *if_cfgs;              /**< key=ifname(strdup), value=isis_if_cfg_t* */
    GHashTable *route_states;         /**< key=ifname|afi(strdup), value=isis_route_state_t* */
    GHashTable *learned_route_heads;  /**< key=learned-route-id(strdup),
                                         value=isis_route_head_t*（多路径，首路径为best且已下发） */
    GHashTable *neighbors;            /**< key=ifname|level|sysid(strdup), value=isis_neighbor_t* */
    GHashTable *lsdb_entries;         /**< key=level|sysid(strdup), value=isis_lsdb_entry_t* */
    isis_nexthop_table_t *nexthop_v4; /**< IPv4 地址组 nexthop registry */
    isis_nexthop_table_t *nexthop_v6; /**< IPv6 地址组 nexthop registry */
    uint32_t lsp_seq_l1;
    uint32_t lsp_seq_l2;
    uint64_t last_lsp_tx_msec;
} isis_instance_cfg_t;

static inline isis_nexthop_table_t *isis_instance_nexthop_table(isis_instance_cfg_t *inst, uint16_t afi)
{
    if (!inst)
    {
        return NULL;
    }
    if (afi == ISIS_AFI_IPV4)
    {
        return inst->nexthop_v4;
    }
    if (afi == ISIS_AFI_IPV6)
    {
        return inst->nexthop_v6;
    }
    return NULL;
}

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
    ISIS_APPLY_OP_COST_STYLE_SET = 9,
} isis_apply_op_t;

/** 应用命令执行结果码（与 BGP 对齐） */
typedef enum isis_apply_rc
{
    ISIS_APPLY_RC_OK = 0,    /**< 成功，已应用，CLI 线程需写 DB */
    ISIS_APPLY_RC_NOOP = 1,  /**< 同配置或可忽略，CLI 线程无需写 DB */
    ISIS_APPLY_RC_FAIL = -1, /**< 参数或状态错误，errmsg 已填写 */
} isis_apply_rc_t;

typedef struct isis_apply_cmd
{
    isis_apply_op_t op;
    isis_apply_rc_t rc;
    char errmsg[256]; /**< 失败时的错误描述（CLI 直接回显） */

    union
    {
        struct
        {
            uint32_t tag;
            uint32_t vrf_id;
            char vrf_name[IF_VRF_NAME_MAX];
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
        struct
        {
            uint32_t tag;
            uint8_t cost_style;
        } cost_style_set;
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

gboolean isis_if_entry_matches_instance(const isis_instance_cfg_t *inst, const if_api_cache_entry_t *entry);

int isis_worker_prepare(void);
int isis_worker_launch(void);
void isis_worker_shutdown(void);

int isis_worker_post_show_cli(dev_ipc_message_t *msg);
int isis_worker_post_if_event(dev_ipc_message_t *msg);
int isis_worker_post_route_ready(void);

/**
 * @brief IPC 线程通知 worker：IF 模块下线，执行 IF 缓存清空 + 邻接撤销 + 路由收回。
 * @return ERRCODE_SUCCESS 成功投递；失败返回 ERRCODE_FAIL。
 */
int isis_worker_post_if_down(void);

int isis_worker_dispatch_apply(isis_apply_cmd_t *apply);

/** worker 线程内：按 tag 查找 instance（cfg_apply 使用） */
isis_instance_cfg_t *isis_lookup_instance(uint32_t tag);
/** worker 线程内：按 tag 获取或创建 instance（cfg_apply 使用） */
isis_instance_cfg_t *isis_get_or_create_instance(uint32_t tag);

#endif /* ISIS_WORKER_H */
