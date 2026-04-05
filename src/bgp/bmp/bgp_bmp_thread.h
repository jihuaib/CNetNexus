/**
 * @file   bgp_bmp_thread.h
 * @brief  BMP 独立线程公共 API：生命周期、事件通知、配置派发
 * @author jhb
 * @date   2026/04/04
 */
#ifndef BGP_BMP_THREAD_H
#define BGP_BMP_THREAD_H

#include <glib.h>
#include <pthread.h>
#include <stdint.h>

#include "dev.h"
#include "net_addr.h"

/* 前向声明 */
struct bgp_session;
typedef struct bgp_session bgp_session_t;
struct bgp_apply_cmd;
typedef struct bgp_apply_cmd bgp_apply_cmd_t;
struct bgp_bmp_instance;
typedef struct bgp_bmp_instance bgp_bmp_instance_t;

/** BMP 实例名最大长度（含 '\0'） */
#define BGP_BMP_INST_NAME_MAX 32

// ============================================================================
// BMP Peer Down 原因码（RFC 7854，供 BGP worker 线程调用 notify 时使用）
// ============================================================================

#define BGP_BMP_PEER_DOWN_LOCAL_NOTIFY 1     /**< 本地发送 NOTIFICATION */
#define BGP_BMP_PEER_DOWN_LOCAL_NO_NOTIFY 2  /**< 本地关闭，未发送 NOTIFICATION */
#define BGP_BMP_PEER_DOWN_REMOTE_NOTIFY 3    /**< 远端发送 NOTIFICATION */
#define BGP_BMP_PEER_DOWN_REMOTE_NO_NOTIFY 4 /**< 远端关闭，未收到 NOTIFICATION */

// ============================================================================
// BGP Session 快照（跨线程安全，纯值类型，无指针）
// ============================================================================

/**
 * @brief BMP 使用的 BGP 会话信息快照
 *
 * 由 BGP worker 线程创建并拷贝到 BMP 线程，
 * 包含构建 BMP Per-Peer Header 和 Peer Up 消息所需的全部字段。
 */
typedef struct bgp_bmp_peer_info
{
    net_addr_t neighbor_addr;   /**< 邻居 IP 地址 */
    net_addr_t source_addr;     /**< 本地源地址 */
    uint32_t remote_as;         /**< 对端 AS 号 */
    uint32_t remote_id;         /**< 对端 BGP Router ID */
    uint16_t remote_hold;       /**< 对端 Hold Time */
    gint64 established_at_usec; /**< 建连时间戳（g_get_real_time） */
    uint32_t local_as;          /**< 本地 AS 号 */
    uint16_t local_hold_time;   /**< 本地 Hold Time */
    uint32_t local_router_id;   /**< 本地 BGP Router ID */
} bgp_bmp_peer_info_t;

// ============================================================================
// BMP 线程本地状态
// ============================================================================

/**
 * @brief BMP 线程本地上下文
 */
typedef struct bgp_bmp_local
{
    int epoll_fd;              /**< BMP 线程 epoll fd */
    int running;               /**< 线程运行标志 */
    pthread_t thread;          /**< BMP 线程句柄 */
    int cmd_eventfd;           /**< 命令唤醒 eventfd */
    GAsyncQueue *cmd_queue;    /**< 命令队列 */
    GHashTable *bmp_instances; /**< BMP 实例运行态，key=实例名字符串 */
} bgp_bmp_local_t;

extern bgp_bmp_local_t *g_bgp_bmp_local;

// ============================================================================
// 生命周期 API（BGP 主线程调用）
// ============================================================================

/**
 * @brief 初始化 BMP 线程资源（epoll + 命令通道）
 * @return ERRCODE_SUCCESS / ERRCODE_FAIL
 */
int bgp_bmp_thread_prepare(void);

/**
 * @brief 启动 BMP 线程
 * @return ERRCODE_SUCCESS / ERRCODE_FAIL
 */
int bgp_bmp_thread_launch(void);

/**
 * @brief 停止 BMP 线程（发送 shutdown、join、释放资源）
 */
void bgp_bmp_thread_shutdown(void);

// ============================================================================
// 配置应用派发（BGP 主线程调用，同步阻塞）
// ============================================================================

/**
 * @brief 向 BMP 线程同步派发配置应用命令
 *
 * 函数阻塞直到 BMP 线程完成处理。返回后 apply->rc 和输出字段已填写。
 * @return 0 成功提交并处理，-1 提交失败（队列不可用）
 */
int bgp_bmp_dispatch_apply(bgp_apply_cmd_t *apply);

/**
 * @brief 向 BMP 线程同步派发“清空所有 BMP 实例”命令
 *
 * 用于 `no bgp` 级联清理，确保 BMP 运行态实例被销毁。
 * @return 0 成功，-1 失败
 */
int bgp_bmp_dispatch_clear_all(void);

// ============================================================================
// BGP 事件通知 API（BGP worker 线程调用，异步）
// ============================================================================

/**
 * @brief 通知 BMP 线程：邻居进入 ESTABLISHED
 *
 * 从 sess 创建快照后投递到 BMP 线程队列，不阻塞。
 * @param sess BGP 会话（BGP worker 线程内有效的指针）
 */
void bgp_bmp_thread_notify_peer_up(bgp_session_t *sess);

/**
 * @brief 通知 BMP 线程：邻居离开 ESTABLISHED
 * @param sess   BGP 会话
 * @param reason Peer Down 原因码
 */
void bgp_bmp_thread_notify_peer_down(bgp_session_t *sess, uint8_t reason);

/**
 * @brief 通知 BMP 线程：收到 BGP UPDATE 报文
 * @param sess    BGP 会话
 * @param bgp_pdu 原始 BGP UPDATE PDU（含 19 字节 header）
 * @param pdu_len PDU 长度
 */
void bgp_bmp_thread_notify_route_monitoring(bgp_session_t *sess, const uint8_t *bgp_pdu, uint16_t pdu_len);

// ============================================================================
// Show 命令转发（BGP 主线程调用，异步）
// ============================================================================

/**
 * @brief 将 BMP show 命令转发到 BMP 线程处理
 * @param msg IPC 消息（所有权转移给 BMP 线程）
 * @return 0 成功，-1 失败
 */
int bgp_bmp_thread_post_show(dev_ipc_message_t *msg);

// ============================================================================
// Initial Peer Ups 支持（BMP 线程 → BGP worker 线程）
// ============================================================================

/**
 * @brief 请求 BGP worker 收集所有 ESTABLISHED 邻居并回传 peer_up 事件
 *
 * BMP 实例连接成功后调用，异步请求。
 * @param inst_name 目标 BMP 实例名（用于回传时路由到正确实例）
 */
void bgp_bmp_request_initial_peers(const char *inst_name);

/**
 * @brief 从 BGP session 创建 peer info 快照
 *
 * 在 BGP worker 线程中调用，安全读取 sess 字段。
 * @param sess BGP 会话
 * @param info 输出快照
 */
void bgp_bmp_fill_peer_info(bgp_session_t *sess, bgp_bmp_peer_info_t *info);

/**
 * @brief 向 BMP 线程投递一条 Initial Peer Up 事件（BGP worker 线程调用）
 *
 * @param peer      邻居快照
 * @param inst_name 目标 BMP 实例名
 */
void bgp_bmp_post_initial_peer_up(const bgp_bmp_peer_info_t *peer, const char *inst_name);

#endif /* BGP_BMP_THREAD_H */
