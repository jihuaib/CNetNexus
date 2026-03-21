/**
 * @file   bgp_fsm.h
 * @brief  BGP 有限状态机（RFC 4271 §8）状态、事件定义与统一事件入口
 * @author jhb
 * @date   2026/03/21
 */
#ifndef BGP_FSM_H
#define BGP_FSM_H

#include <stdint.h>

/* 前向声明，避免循环包含 */
struct bgp_session;
struct bgp_conn;

/**
 * @brief BGP FSM 状态（RFC 4271 §8.2.2）
 *
 * 枚举值与 RFC 4271 Table 8 状态名称一一对应，顺序与 RFC 保持一致。
 * IDLE 为初始状态；ESTABLISHED 为正常路由交换状态。
 */
typedef enum bgp_fsm_state
{
    BGP_FSM_STATE_IDLE = 0,         /**< 空闲：未分配连接资源，等待 Start 事件 */
    BGP_FSM_STATE_CONNECT = 1,      /**< 连接中：主动 TCP 握手正在进行（EPOLLOUT 等待） */
    BGP_FSM_STATE_ACTIVE = 2,       /**< 主动：ConnectRetry 定时器运行中，等待重连机会 */
    BGP_FSM_STATE_OPEN_SENT = 3,    /**< OpenSent：OPEN 已发出，等待对端 OPEN 报文 */
    BGP_FSM_STATE_OPEN_CONFIRM = 4, /**< OpenConfirm：已收到对端 OPEN，等待对端 KEEPALIVE */
    BGP_FSM_STATE_ESTABLISHED = 5,  /**< 已建立：正常路由交换运行状态 */
    BGP_FSM_STATE_MAX = 6,
} bgp_fsm_state_t;

/**
 * @brief BGP FSM 事件（RFC 4271 §8.1 Event Numbers）
 *
 * 枚举值严格与 RFC 4271 事件编号对应，便于对照规范实现和调试。
 * 标注"暂不实现"的可选事件已保留枚举值，收到后记录警告并按默认处理。
 */
typedef enum bgp_fsm_event
{
    /* ---- 管理事件 Events 1-8 ---- */
    BGP_EVT_MANUAL_START = 1,            /**< Event 1: 手动启动 BGP 会话 */
    BGP_EVT_MANUAL_STOP = 2,             /**< Event 2: 手动停止 BGP 会话（不重连） */
    BGP_EVT_AUTO_START = 3,              /**< Event 3: 自动启动（AF 邻居配置完成时触发） */
    BGP_EVT_MANUAL_START_PASSIVE = 4,    /**< Event 4: 被动模式手动启动（可选，暂不实现） */
    BGP_EVT_AUTO_START_PASSIVE = 5,      /**< Event 5: 被动模式自动启动（可选，暂不实现） */
    BGP_EVT_AUTO_START_DAMP = 6,         /**< Event 6: 带振荡抑制的自动启动（可选，暂不实现） */
    BGP_EVT_AUTO_START_DAMP_PASSIVE = 7, /**< Event 7: 振荡抑制+被动模式（可选，暂不实现） */
    BGP_EVT_AUTO_STOP = 8,               /**< Event 8: 自动停止（配置删除时触发，不重连） */

    /* ---- 定时器事件 Events 9-13 ---- */
    BGP_EVT_CONNECT_RETRY_EXPIRED = 9,     /**< Event 9: ConnectRetry 定时器超时，触发重连 */
    BGP_EVT_HOLD_TIMER_EXPIRED = 10,       /**< Event 10: HoldTimer 超时，发送 NOTIFICATION 后关闭 */
    BGP_EVT_KEEPALIVE_TIMER_EXPIRED = 11,  /**< Event 11: KeepaliveTimer 周期触发，发送 KEEPALIVE */
    BGP_EVT_DELAY_OPEN_TIMER_EXPIRED = 12, /**< Event 12: DelayOpenTimer 超时（可选，暂不实现） */
    BGP_EVT_IDLE_HOLD_TIMER_EXPIRED = 13,  /**< Event 13: IdleHoldTimer 超时（可选，暂不实现） */

    /* ---- TCP 连接事件 Events 14-17 ---- */
    BGP_EVT_TCP_CR_ACKED = 14,             /**< Event 14: 主动 TCP 握手成功（SO_ERROR=0） */
    BGP_EVT_TCP_CONNECTION_CONFIRMED = 15, /**< Event 15: 被动 TCP 连接已建立（accept 成功） */
    BGP_EVT_TCP_CR_FATAL = 16,             /**< Event 16: TCP 连接致命错误（不可重试） */
    BGP_EVT_TCP_CONNECTION_FAILS = 17,     /**< Event 17: TCP 连接失败（可调度重试） */

    /* ---- BGP 报文事件 Events 19-28 ---- */
    BGP_EVT_BGP_OPEN = 19, /**< Event 19: 收到合法 OPEN（bgp_pkt.c 已发 KEEPALIVE，conn->state=OPEN_CONFIRM） */
    BGP_EVT_BGP_OPEN_WITH_DELAY = 20, /**< Event 20: DelayOpen 期间收到 OPEN（可选，暂不实现） */
    BGP_EVT_BGP_HEADER_ERR = 21,      /**< Event 21: 报文头部格式非法 */
    BGP_EVT_BGP_OPEN_MSG_ERR = 22,    /**< Event 22: OPEN 报文内容非法 */
    BGP_EVT_OPEN_COLLISION_DUMP = 23, /**< Event 23: RFC §6.8 碰撞检测—应关闭当前连接 */
    BGP_EVT_NOTIF_MSG_VER_ERR = 24,   /**< Event 24: 收到版本错误 NOTIFICATION（不重试） */
    BGP_EVT_NOTIF_MSG = 25,           /**< Event 25: 收到 NOTIFICATION 报文（关闭并重试） */
    BGP_EVT_KEEPALIVE_MSG = 26,       /**< Event 26: 收到 KEEPALIVE 报文 */
    BGP_EVT_UPDATE_MSG = 27,          /**< Event 27: 收到 UPDATE 报文（hold timer 重置） */
    BGP_EVT_UPDATE_MSG_ERR = 28,      /**< Event 28: 收到非法 UPDATE 报文 */

    BGP_EVT_MAX = 29,
} bgp_fsm_event_t;

/**
 * @brief BGP FSM 统一事件入口
 *
 * 根据当前 sess->fsm_state 和 evt，执行对应动作并转移到新状态。
 * 所有状态变迁（定时器操作、连接管理、报文发送）均经此函数完成。
 * 调用方无需关心当前状态，FSM 自动选择正确的处理路径。
 *
 * conn 参数使用说明：
 *   - TCP/报文事件（14-28）：传入触发该事件的具体连接（可为 pri_conn 或 sec_conn）
 *   - 管理/定时器事件（1-13）：传入 sess->pri_conn 或 NULL
 *
 * @param sess     目标会话（不可为 NULL）
 * @param conn     触发事件的连接对象（管理/定时器事件可为 NULL）
 * @param evt      RFC 4271 事件类型
 * @param epoll_fd BGP worker 的 epoll fd（用于定时器和连接的 epoll 操作）
 */
void bgp_fsm_event(struct bgp_session *sess, struct bgp_conn *conn, bgp_fsm_event_t evt, int epoll_fd);

/**
 * @brief 返回 FSM 状态名称字符串（用于日志）
 * @param state FSM 状态
 * @return 状态名称常量字符串（不可修改，不可释放）
 */
const char *bgp_fsm_state_str(bgp_fsm_state_t state);

/**
 * @brief 返回 FSM 事件名称字符串（用于日志）
 * @param evt FSM 事件
 * @return 事件名称常量字符串（不可修改，不可释放）
 */
const char *bgp_fsm_event_str(bgp_fsm_event_t evt);

#endif /* BGP_FSM_H */
