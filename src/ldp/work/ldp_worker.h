/**
 * @file   ldp_worker.h
 * @brief  LDP worker 线程：协议状态机、定时器、UDP 发现与 show 处理
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_WORKER_H
#define LDP_WORKER_H

#include <glib.h>
#include <pthread.h>
#include <stdint.h>

#include "dev.h"
#include "if.h"
#include "ldp.h"
#include "ldp_db.h"

#define LDP_MAX_EPOLL_EVENTS 32

/**
 * @brief 单个使能 LDP 的接口的运行时状态
 */
typedef struct ldp_iface_state
{
    char ifname[IF_LOGICAL_NAME_MAX];
    uint32_t ifindex;            /**< 0 表示尚未从 IF cache 学到 */
    uint32_t ipv4_local;         /**< 本端 IPv4 主地址（host order）；0 表示未知 */
    uint8_t enabled;             /**< 配置是否使能 */
    uint8_t link_up;             /**< 链路与协议层是否就绪 */
    uint8_t multicast_joined;    /**< 是否已加入 224.0.0.2 */
    int udp_fd;                  /**< UDP 646 socket；-1 表示未建立 */
    uint32_t hello_interval_ms;  /**< 0 表示继承全局值 */
    uint32_t hold_time_ms;       /**< 0 表示继承全局值 */
    uint64_t last_hello_tx_msec; /**< 上次发送 hello 的时间戳 */
} ldp_iface_state_t;

/**
 * @brief LDP 邻接（hello 维持），key=(peer_lsr_id<<16 | peer_label_space)
 */
typedef struct ldp_adjacency
{
    char ifname[IF_LOGICAL_NAME_MAX]; /**< 学到 hello 的接口 */
    uint32_t peer_lsr_id;             /**< 对端 LSR-ID（IPv4 host order） */
    uint16_t peer_label_space;        /**< 对端标签空间 */
    uint32_t peer_transport_v4;       /**< 对端 transport addr（host order） */
    uint32_t peer_hello_hold_ms;      /**< 对端通告的 hold 时间 */
    uint32_t neg_hold_ms;             /**< 双方协商后的 hold（min） */
    uint32_t configuration_seq;       /**< 对端 Configuration Sequence Number */
    uint64_t last_seen_msec;
} ldp_adjacency_t;

/**
 * @brief LDP worker 共享状态
 */
typedef struct ldp_work_local
{
    ldp_proto_cfg_t proto;   /**< 协议级配置（admin/lsr_id/各定时器） */
    GHashTable *interfaces;  /**< key=ifname strdup, value=ldp_iface_state_t* */
    GHashTable *adjacencies; /**< key=guint64 packed, value=ldp_adjacency_t* */
    GHashTable *peers;       /**< key=guint64 packed, value=ldp_peer_t* */

    int epoll_fd;
    int cmd_eventfd;
    int tick_fd;       /**< timerfd, 1Hz */
    int tcp_listen_fd; /**< TCP 646 监听 socket */
    pthread_t thread;
    volatile int running;
    GAsyncQueue *cmd_queue;
} ldp_work_local_t;

// ============================================================================
// epoll 事件分发：用 (kind << 32) | fd 编码 ev.data.u64，分发时按 fd 反查
// peer/iface，避免直接传指针导致的 use-after-free。
// ============================================================================

typedef enum ldp_evt_kind
{
    LDP_EVT_KIND_NONE = 0,
    LDP_EVT_KIND_CMD = 1,
    LDP_EVT_KIND_TICK = 2,
    LDP_EVT_KIND_LISTEN = 3,
    LDP_EVT_KIND_IFACE = 4,
    LDP_EVT_KIND_PEER = 5,
    LDP_EVT_KIND_PENDING_ACCEPT = 6, /**< 已 accept 但还没匹配到 adjacency 的 fd */
} ldp_evt_kind_t;

#define LDP_EVT_PACK(kind, fd) (((uint64_t)(uint32_t)(kind) << 32) | (uint64_t)(uint32_t)(fd))
#define LDP_EVT_KIND(u64) ((ldp_evt_kind_t)((uint32_t)((u64) >> 32)))
#define LDP_EVT_FD(u64) ((int)(int32_t)((u64) & 0xFFFFFFFFu))

extern ldp_work_local_t *g_ldp_work_local;

// ============================================================================
// apply 命令
// ============================================================================

typedef enum ldp_apply_op
{
    LDP_APPLY_OP_PROTO_SET = 1, /**< 全局协议参数（admin/lsr_id/timers）覆盖 */
    LDP_APPLY_OP_IF_SET = 2,    /**< 接口使能或参数变更 */
    LDP_APPLY_OP_IF_DEL = 3,    /**< 接口去使能 */
} ldp_apply_op_t;

typedef struct ldp_apply_cmd
{
    ldp_apply_op_t op;
    int rc;

    union
    {
        ldp_proto_cfg_t proto;
        ldp_if_cfg_t if_set;
        struct
        {
            char ifname[IF_LOGICAL_NAME_MAX];
        } if_del;
    } u;
} ldp_apply_cmd_t;

// ============================================================================
// 生命周期
// ============================================================================

int ldp_worker_prepare(void);
int ldp_worker_launch(void);
void ldp_worker_shutdown(void);

int ldp_worker_post_show_cli(dev_ipc_message_t *msg);
int ldp_worker_post_if_event(dev_ipc_message_t *msg);
int ldp_worker_post_route_msg(dev_ipc_message_t *msg);
int ldp_worker_dispatch_apply(ldp_apply_cmd_t *apply);

// ============================================================================
// 工具：当前毫秒、有效定时器值、host-order LSR-ID 转点分十进制
// ============================================================================

uint64_t ldp_worker_now_msec(void);

uint32_t ldp_worker_effective_hello_ms(const ldp_iface_state_t *iface);
uint32_t ldp_worker_effective_hold_ms(const ldp_iface_state_t *iface);

void ldp_worker_format_lsr_id(uint32_t lsr_id_host, char *buf, size_t buf_len);

guint64 ldp_worker_adj_key(uint32_t peer_lsr_id, uint16_t label_space);

#endif /* LDP_WORKER_H */
