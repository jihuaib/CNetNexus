/**
 * @file   ldp_session.h
 * @brief  LDP TCP session 状态机（RFC 5036 §2.5）
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_SESSION_H
#define LDP_SESSION_H

#include "ldp_worker.h"

#define LDP_SESSION_RX_BUF_CAP 4096u
#define LDP_SESSION_TX_BUF_CAP 4096u

typedef enum ldp_peer_state
{
    LDP_PEER_NON_EXIST = 0,
    LDP_PEER_INITIALIZED = 1, /**< TCP 已建立，主动方刚发完 Init / 被动方刚收到 Init */
    LDP_PEER_OPEN_SENT = 2,   /**< 主动方等待 Init 应答 */
    LDP_PEER_OPEN_REC = 3,    /**< 被动方已发 Init+KA，等待 KA */
    LDP_PEER_OPEN_CONFIRM = 4,
    LDP_PEER_OPERATIONAL = 5,
} ldp_peer_state_t;

typedef struct ldp_peer
{
    uint32_t peer_lsr_id; /**< host order */
    uint16_t peer_label_space;
    uint8_t is_active; /**< 1 = 主动方 */
    int fd;            /**< TCP socket；-1 未连接 */
    ldp_peer_state_t state;

    uint8_t rx_buf[LDP_SESSION_RX_BUF_CAP];
    size_t rx_len;
    uint8_t tx_buf[LDP_SESSION_TX_BUF_CAP];
    size_t tx_len;

    uint32_t next_msg_id;
    uint32_t our_keepalive_ms;  /**< 本端配置 */
    uint32_t peer_keepalive_ms; /**< 对端通告 */
    uint32_t neg_keepalive_ms;  /**< min(本端, 对端) */
    uint64_t last_rx_msec;
    uint64_t last_tx_msec;
    uint64_t connecting_since_msec; /**< active 方发起 connect() 的时间，用于退避 */

    /* RFC 5036 §2.5.3：active 端连接需要等双方 hello 都互相到达后再发起，
     * 否则一方先收到 hello 立刻 connect、对端还没有 adjacency 槽，导致 passive
     * 端 reject。这里把 transport 地址和首次 adjacency 时间缓存下来，由 tick
     * 在延迟（LDP_INIT_CONNECT_DELAY_MS）后才真正 connect_active。*/
    uint32_t peer_transport_v4;   /**< adjacency 通告的对端 transport 地址（host order） */
    uint32_t self_transport_v4;   /**< 本端用于该 adjacency 的 transport 地址（host order） */
    uint32_t peer_link_addr_v4;   /**< adjacency 中 hello 包源 IP（对端直连接口, host order） */
    uint64_t adj_first_seen_msec; /**< 第一次为这个 peer 学到 adjacency 的时间 */
} ldp_peer_t;

/* 由 discovery 在邻接出现/消失时调用 */
void ldp_session_on_adjacency_up(uint32_t peer_lsr_id, uint16_t peer_label_space, uint32_t peer_transport_v4,
                                 uint32_t self_transport_v4, uint32_t peer_link_addr_v4);
void ldp_session_on_adjacency_down(uint32_t peer_lsr_id, uint16_t peer_label_space);

/* 工作线程内部入口 */
int ldp_session_open_listener(void);
void ldp_session_close_listener(void);
void ldp_session_handle_listen_accept(void);
void ldp_session_handle_io(ldp_peer_t *peer, uint32_t epoll_events);
void ldp_session_tick(void);
void ldp_session_close(ldp_peer_t *peer, const char *reason);
void ldp_session_close_all(void);

ldp_peer_t *ldp_session_lookup(uint32_t peer_lsr_id, uint16_t peer_label_space);

const char *ldp_session_state_str(ldp_peer_state_t st);

/* ---- pending passive accept ---- */
/**
 * @brief 处理 KIND_PENDING_ACCEPT 类型 fd 上的 epoll 事件（关闭/EOF）。
 */
void ldp_session_pending_handle_io(int fd, uint32_t events);

/**
 * @brief 邻接出现后，把 transport 地址匹配的 pending 连接交给 peer。
 *        由 discovery 在新建/刷新 adjacency 时调用。
 */
void ldp_session_pending_promote_for_transport(uint32_t transport_v4);

#endif /* LDP_SESSION_H */
