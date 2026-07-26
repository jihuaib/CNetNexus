/**
 * @file   dev.h
 * @brief  DEV 模块公共接口，包含模块 ID、DEV RPC 与 DEV IPC 公共 API
 * @author jhb
 * @date   2026/03/03
 */

#ifndef DEV_H
#define DEV_H

#include <glib.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// ============================================================================
// 模块 ID 定义
// ============================================================================

/** DEV 模块 */
#define DEV_MODULE_ID_DEV 0x00000001
/** DB 模块 */
#define DEV_MODULE_ID_DB 0x00000002
/** CFG 模块 */
#define DEV_MODULE_ID_CLI 0x00000003
/** VRF 模块 */
#define DEV_MODULE_ID_VRF 0x00000004
/** IF 接口模块 */
#define DEV_MODULE_ID_IF 0x00000005
/** BGP 模块 */
#define DEV_MODULE_ID_BGP 0x00000006
/** Route 模块 */
#define DEV_MODULE_ID_ROUTE 0x00000007
/** SBMP（BMP Server）模块 */
#define DEV_MODULE_ID_SBMP 0x00000008
/** ISIS 模块 */
#define DEV_MODULE_ID_ISIS 0x00000009
/** Tunnel 模块 */
#define DEV_MODULE_ID_TUNNEL 0x0000000A
/** FIB 模块 */
#define DEV_MODULE_ID_FIB 0x0000000B
/** LDP 模块 */
#define DEV_MODULE_ID_LDP 0x0000000C
/** ACCESS 接入层模块（line 层：telnet/串口/ssh 接入） */
#define DEV_MODULE_ID_ACCESS 0x0000000D
/** LLDP 模块 */
#define DEV_MODULE_ID_LLDP 0x0000000E
/** SNMP 模块 */
#define DEV_MODULE_ID_SNMP 0x0000000F
/** OSPFv2 模块 */
#define DEV_MODULE_ID_OSPF 0x00000010
/** OSPFv3 模块 */
#define DEV_MODULE_ID_OSPFV3 0x00000011

/** 无效文件描述符 */
#define DEV_INVALID_FD (-1)

/** 模块名称最大长度（DEV RPC 返回值缓冲区） */
#define DEV_MODULE_NAME_MAX_LEN 12

// ============================================================================
// 各模块 IPC 监听端口配置
// ============================================================================

/** 本地回环地址 */
#define DEV_IPC_HOST_LOCAL "127.0.0.1"

/** DEV 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_DEV 4001
/** DB 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_DB 4002
/** CFG 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_CLI 4003
/** IF 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_IF 4004
/** BGP 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_BGP 4005
/** ROUTE 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_ROUTE 4006
/** VRF 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_VRF 4007
/** SBMP 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_SBMP 4008
/** ISIS 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_ISIS 4009
/** TUNNEL 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_TUNNEL 4010
/** FIB 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_FIB 4011
/** LDP 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_LDP 4012
/** ACCESS 接入层模块 IPC 监听端口 */
#define DEV_MODULE_PORT_ACCESS 4013
/** LLDP 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_LLDP 4014
/** SNMP 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_SNMP 4015
/** OSPFv2 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_OSPF 4016
/** OSPFv3 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_OSPFV3 4017

// ============================================================================
// IPC 前向声明
// ============================================================================

typedef struct dev_ipc_context dev_ipc_context_t;
typedef struct dev_ipc_message dev_ipc_message_t;

/**
 * @brief DEV IPC 消息处理回调函数类型
 * @param ctx IPC 上下文
 * @param msg 接收到的消息
 */
typedef void (*dev_ipc_msg_handler_fn)(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
typedef dev_ipc_msg_handler_fn dev_ipc_msg_handler_fn;

/**
 *  DEV IPC 连接断开回调函数类型
 *  ctx IPC 上下文
 *  remote_module_id 断开的对端模块 ID
 *  user 用户数据
 */
typedef void (*dev_ipc_disconnect_handler_fn)(dev_ipc_context_t *ctx, uint32_t remote_module_id, void *user);
typedef dev_ipc_disconnect_handler_fn dev_ipc_disconnect_handler_fn;

// ============================================================================
// DEV IPC 消息类型编码：msg_type = (大类 << 16) | 子类
// ============================================================================

/** 提取消息大类（高 16 位） */
#define DEV_IPC_MSG_CATEGORY(t) (((t) >> 16) & 0xFFFF)
/** 提取消息子类（低 16 位） */
#define DEV_IPC_MSG_SUBTYPE(t) ((t) & 0xFFFF)
/** 由大类和子类构造 msg_type */
#define DEV_IPC_MSG_TYPE(cat, sub) (((uint32_t)(cat) << 16) | ((uint32_t)(sub) & 0xFFFF))

// ============================================================================
// DEV IPC 消息大类定义（与模块 ID 对齐）
// ============================================================================

/** DEV IPC 内部消息大类 */
#define DEV_IPC_CATEGORY_INTERNAL 0x0000
/** DEV 模块消息大类 */
#define DEV_IPC_CATEGORY_DEV 0x0001
/** DB 模块消息大类 */
#define DEV_IPC_CATEGORY_DB 0x0002
/** CLI 模块消息大类 */
#define DEV_IPC_CATEGORY_CLI 0x0003
/** IF 模块消息大类 */
#define DEV_IPC_CATEGORY_IF 0x0004
/** BGP 模块消息大类 */
#define DEV_IPC_CATEGORY_BGP 0x0005
/** ROUTE 模块消息大类 */
#define DEV_IPC_CATEGORY_ROUTE 0x0006
/** VRF 模块消息大类 */
#define DEV_IPC_CATEGORY_VRF 0x0007
/** SBMP 模块消息大类 */
#define DEV_IPC_CATEGORY_SBMP 0x0008
/** ISIS 模块消息大类 */
#define DEV_IPC_CATEGORY_ISIS 0x0009
/** TUNNEL 模块消息大类 */
#define DEV_IPC_CATEGORY_TUNNEL 0x000A
/** FIB 模块消息大类 */
#define DEV_IPC_CATEGORY_FIB 0x000B
/** LDP 模块消息大类 */
#define DEV_IPC_CATEGORY_LDP 0x000C
/** ACCESS 接入层模块消息大类 */
#define DEV_IPC_CATEGORY_ACCESS 0x000D
/** LLDP 模块消息大类 */
#define DEV_IPC_CATEGORY_LLDP 0x000E
/** SNMP 模块消息大类 */
#define DEV_IPC_CATEGORY_SNMP 0x000F
/** OSPFv2 模块消息大类 */
#define DEV_IPC_CATEGORY_OSPF 0x0010
/** OSPFv3 模块消息大类 */
#define DEV_IPC_CATEGORY_OSPFV3 0x0011

// ============================================================================
// DEV IPC 消息结构
// ============================================================================

/** DEV IPC 魔数 "NNIP" */
#define DEV_IPC_MAGIC 0x4E4E4950

/** DEV IPC 帧头部大小（网络传输时只序列化前 24 字节） */
#define DEV_IPC_FRAME_HEADER_SIZE 24
/** request_id 高位作为同步查询响应标记，低 31 位为查询 ID */
#define DEV_IPC_REQUEST_ID_RESPONSE_FLAG 0x80000000u
#define DEV_IPC_REQUEST_ID_MASK 0x7FFFFFFFu

/**
 * @brief 统一 DEV IPC 消息结构
 *
 * 前 24 字节（magic 到 payload_len）用于网络传输，
 * payload 和 free_fn 仅在内存中使用。
 */
struct dev_ipc_message
{
    uint32_t magic;                  /**< 魔数 0x4E4E4950 ("NNIP") */
    uint32_t msg_type;               /**< 消息类型 = (大类 << 16) | 子类 */
    uint32_t src_module_id;          /**< 源模块 ID */
    uint32_t dst_module_id;          /**< 目标模块 ID */
    uint32_t request_id;             /**< 请求 ID，用于请求/响应配对 */
    uint32_t payload_len;            /**< 负载长度 */
    void *payload;                   /**< 负载数据（内存中使用） */
    void (*free_fn)(void *);         /**< 负载释放函数（内存中使用） */
    uint32_t ingress_peer_module_id; /**< 接收连接握手确认的对端 ID；仅本地元数据，不上线路 */
    uint8_t ingress_on_initiator;    /**< 消息是否来自本模块主动建立的连接；仅本地元数据 */
};

// ============================================================================
// DEV IPC 连接状态
// ============================================================================

/** DEV IPC 连接状态枚举 */
typedef enum dev_ipc_costate
{
    DEV_IPC_CODISCONNECTED = 0, /**< 未连接 */
    DEV_IPC_COCONNECTING,       /**< 连接中 */
    DEV_IPC_COHANDSHAKING,      /**< 握手中 */
    DEV_IPC_COCONNECTED,        /**< 已连接 */
    DEV_IPC_CORECONNECTING      /**< 重连中 */
} dev_ipc_costate_t;

typedef dev_ipc_costate_t dev_ipc_costate_t;

// ============================================================================
// DEV IPC 配置
// ============================================================================

/** 同步查询默认超时（毫秒） */
#ifdef NDEBUG
/* Release 模式：正常心跳参数 */
#    define DEV_IPC_HEARTBEAT_INTERVAL 5
#    define DEV_IPC_HEARTBEAT_TIMEOUT 15
#    define DEV_IPC_CONNECT_TIMEOUT 2
#    define DEV_IPC_QUERY_TIMEOUT_DEFAULT 5000
/* 模块层等待 peer / DEV / module-ready 的超时（毫秒） */
#    define DEV_IPC_WAIT_PEER_MS 3000
#    define DEV_IPC_WAIT_DEV_MS 10000
#    define DEV_IPC_WAIT_READY_MS 15000
/* 控制面 RPC 单次超时（毫秒） */
#    define DEV_IPC_SUBSCRIBE_RPC_MS 2000
#else
/* Debug 模式：延长心跳和超时，方便 GDB 调试 */
#    define DEV_IPC_HEARTBEAT_INTERVAL 300
#    define DEV_IPC_HEARTBEAT_TIMEOUT 600
#    define DEV_IPC_CONNECT_TIMEOUT 30
#    define DEV_IPC_QUERY_TIMEOUT_DEFAULT 30000
/* Debug 下 DEV 的 connect 兜底 30s，模块层等待必须 ≥ 它 + 余量，
 * 否则 vrf 类模块会在 DEV 还没连过来时就放弃，后续 SUBSCRIBE/notify_ready 全部翻车 */
#    define DEV_IPC_WAIT_PEER_MS 45000
#    define DEV_IPC_WAIT_DEV_MS 45000
#    define DEV_IPC_WAIT_READY_MS 60000
#    define DEV_IPC_SUBSCRIBE_RPC_MS 10000
#endif

/* 硬约束：模块等 DEV 的窗口必须能覆盖 DEV 自己 connect 的兜底超时 */
_Static_assert(DEV_IPC_WAIT_DEV_MS >= (DEV_IPC_CONNECT_TIMEOUT * 1000 + 5000),
               "DEV_IPC_WAIT_DEV_MS must exceed DEV_IPC_CONNECT_TIMEOUT by at least 5s");

/** 初始重连延迟（毫秒） */
#define DEV_IPC_RECONNECT_DELAY_MIN 500
/** 最大重连延迟（毫秒） */
#define DEV_IPC_RECONNECT_DELAY_MAX 10000
/** 本 socket 的 SYN 重传上限(per-socket TCP_SYNCNT)。
 *  Linux 默认 tcp_syn_retries=6 → ~63s 才 ETIMEDOUT;
 *  设 3 → ~7s(1+2+4),避免 fork→connect 抢跑被 kernel SYN-RETRY 卡住,
 *  和 GDB 调试无关(SYN 处理在内核,断不到用户态)。 */
#define DEV_IPC_TCP_SYN_RETRIES 3
/** 模块名称最大长度 */
#define DEV_IPC_MODULE_NAME_MAX 32
/** 最大连接数 */
#define DEV_IPC_MAX_CONNECTIONS 16
/** 接收缓冲区大小 */
#define DEV_IPC_RECV_BUF_SIZE 65536

// ============================================================================
// DEV IPC 内部数据结构
// ============================================================================

/** 单条 TCP 连接 */
typedef struct dev_ipc_connection
{
    uint32_t remote_module_id; /**< 对端模块 ID */
    int fd;                    /**< socket 文件描述符 */
    dev_ipc_costate_t state;   /**< 连接状态 */

    /* 目标地址（仅主动连接方使用，用于断连后重连） */
    char remote_host[64]; /**< 目标主机地址 */
    uint16_t remote_port; /**< 目标端口 */

    /* 接收缓冲区 */
    uint8_t recv_buf[DEV_IPC_RECV_BUF_SIZE]; /**< 接收缓冲区 */
    uint32_t recv_len;                       /**< 已接收数据长度 */

    /* 心跳 */
    time_t last_heartbeat_sent; /**< 上次发送心跳的时间 */
    time_t last_heartbeat_recv; /**< 上次收到心跳的时间 */

    /* 重连 */
    uint32_t reconnect_delay_ms; /**< 当前重连延迟 */
    time_t next_reconnect_time;  /**< 下次重连时间 */
    int draining;                /**< DEV 已广播 DOWN；仅排空在途响应，不再承载新消息 */

    /* 是否为主动发起方 */
    int is_initiator; /**< 1=主动连接方，0=被接受方 */
} dev_ipc_connection_t;

typedef dev_ipc_connection_t dev_ipc_connection_t;

/** 挂起的同步查询 */
typedef struct dev_ipc_pending_query
{
    uint32_t request_id;         /**< 请求 ID */
    uint32_t target_module_id;   /**< 目标模块 ID（用于按目标取消） */
    dev_ipc_message_t *response; /**< 响应消息（由 IO 线程设置） */
    pthread_cond_t cond;         /**< 条件变量 */
    int completed;               /**< 是否已完成 */
} dev_ipc_pending_query_t;

typedef dev_ipc_pending_query_t dev_ipc_pending_query_t;

/** 查询管理器 */
typedef struct dev_ipc_query_mgr
{
    GHashTable *pending;  /**< request_id -> dev_ipc_pending_query_t* */
    pthread_mutex_t lock; /**< 全局锁 */
    uint32_t next_id;     /**< 下一个请求 ID */
} dev_ipc_query_mgr_t;

typedef dev_ipc_query_mgr_t dev_ipc_query_mgr_t;

/* 前向声明：订阅管理器（细节在 ipc_subscribe.c） */
typedef struct dev_ipc_subscribe_mgr dev_ipc_subscribe_mgr_t;

/** IPC 上下文完整定义 */
struct dev_ipc_context
{
    /* 本模块信息 */
    uint32_t module_id;                 /**< 本模块 ID */
    char name[DEV_IPC_MODULE_NAME_MAX]; /**< 模块名称 */

    /* 消息处理 */
    dev_ipc_msg_handler_fn msg_handler;               /**< 消息处理回调 */
    dev_ipc_disconnect_handler_fn disconnect_handler; /**< 连接断开回调（IO 线程上下文） */
    void *disconnect_user;                            /**< 连接断开回调用户数据 */

    /* 连接 */
    dev_ipc_connection_t *connections[DEV_IPC_MAX_CONNECTIONS]; /**< 连接数组 */
    int num_connections;                                        /**< 连接数 */
    pthread_mutex_t comutex;                                    /**< 连接锁 */
    GHashTable *target_lifecycle_states; /**< target -> 最新 epoch/READY-DOWN 状态（受 comutex 保护） */

    /* 监听 */
    int listen_fd; /**< 监听 socket（本模块 IPC 端口） */

    /* IO 线程 */
    int epoll_fd;                    /**< epoll 文件描述符 */
    pthread_t io_thread;             /**< IO 线程 */
    volatile int running;            /**< 运行标志 */
    volatile int shutdown_requested; /**< 关闭请求标志 */

    /* Worker 线程 */
    GAsyncQueue *msg_queue;  /**< 业务消息队列（IO线程投递，Worker线程消费） */
    pthread_t worker_thread; /**< Worker 线程（执行 msg_handler，可安全调用 dev_ipc_query） */

    /* 同步查询 */
    dev_ipc_query_mgr_t *query_mgr; /**< 查询管理器 */

    /* 订阅管理器（按需启动 / MODULE_EVENT 路由） */
    dev_ipc_subscribe_mgr_t *sub_mgr;

    /* notify_ready 延迟标志:业务调用 dev_ipc_notify_ready 时若 DEV 还未连上,
     * 置位本标志,IO 线程在 handshake 完成时自动补发。
     * 仅 IO 线程读/写 + dev_ipc_notify_ready 写,无锁原子语义足够。 */
    volatile int pending_notify_ready;
};

// ============================================================================
// DEV IPC 全局变量
// ============================================================================

extern dev_ipc_context_t *g_dev_ipc_context;

// ============================================================================
// DEV IPC 内部消息子类（大类 = DEV_IPC_CATEGORY_INTERNAL）
// ============================================================================

/** 握手请求 */
#define DEV_IPC_MSG_TYPE_HANDSHAKE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_INTERNAL, 0x0001)
/** 握手响应 */
#define DEV_IPC_MSG_TYPE_HANDSHAKE_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_INTERNAL, 0x0002)
/** 心跳请求 */
#define DEV_IPC_MSG_TYPE_HEARTBEAT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_INTERNAL, 0x0003)
/** 心跳响应 */
#define DEV_IPC_MSG_TYPE_HEARTBEAT_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_INTERNAL, 0x0004)
/** 关闭通知 */
#define DEV_IPC_MSG_TYPE_SHUTDOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_INTERNAL, 0x0005)

// ============================================================================
// DEV 模块生命周期消息子类（大类 = DEV_IPC_CATEGORY_DEV）
// ============================================================================

/** 查询模块名称 */
#define DEV_IPC_MSG_TYPE_DEV_GET_MODULE_NAME DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0005)
/** 查询目标模块的所有 IPC 连接状态（由 IPC 库层自动处理，无需应用层介入） */
#define DEV_IPC_MSG_TYPE_DEV_QUERY_IPC_CONNS DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0006)
/** QUERY_IPC_CONNS 的响应（msg_type 与请求不同，确保 query_mgr 能正确路由） */
#define DEV_IPC_MSG_TYPE_DEV_QUERY_IPC_CONNS_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0007)
/** 设置目标模块的日志级别（payload = 4 字节 uint32 网络字节序，由 IPC 库层自动处理） */
#define DEV_IPC_MSG_TYPE_DEV_SET_LOG_LEVEL DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0008)
/** 设置目标模块的远端 syslog（payload = syslog_report_remote_config_t，整数字段为网络字节序） */
#define DEV_IPC_MSG_TYPE_DEV_SET_SYSLOG_REMOTE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x000D)
/** 模块阶段响应 */
#define DEV_IPC_MSG_TYPE_DEV_MODULE_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x000F)
/** 查询目标模块的本地 sub_mgr 订阅表（由 IPC 库层自动处理，无需应用层介入） */
#define DEV_IPC_MSG_TYPE_DEV_QUERY_SUBS DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0009)
/** QUERY_SUBS 的响应（payload 为 NUL 结尾的文本 dump） */
#define DEV_IPC_MSG_TYPE_DEV_QUERY_SUBS_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x000A)
/** 模块即将退出（自报）：DEV 同步完成 phase/broadcast/drop_connection 后再 ACK，
 *  调用方收到 ACK 才真正 exit()，避免 SIGCHLD/SUBSCRIBE 抢锁 race。
 *  payload 无（src_module_id 即为退出模块） */
#define DEV_IPC_MSG_TYPE_DEV_PRE_EXIT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x000B)
/** PRE_EXIT 的响应（payload 无） */
#define DEV_IPC_MSG_TYPE_DEV_PRE_EXIT_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x000C)

/* ---------------- 订阅 / 按需启动相关 ---------------- */

/** 订阅目标模块就绪事件（payload=dev_subscribe_req_t；响应=dev_subscribe_resp_t） */
#define DEV_IPC_MSG_TYPE_DEV_SUBSCRIBE_MODULE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0010)
/** 取消订阅（payload=4B target_module_id 网络字节序；无响应） */
#define DEV_IPC_MSG_TYPE_DEV_UNSUBSCRIBE_MODULE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0011)
/** 模块通知 DEV "我已就绪"（payload=无；触发 DEV 推送 MODULE_EVENT 给订阅者） */
#define DEV_IPC_MSG_TYPE_DEV_NOTIFY_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0012)
/** DEV → 订阅者推送的模块事件（payload=dev_module_event_payload_t；单向，无响应） */
#define DEV_IPC_MSG_TYPE_DEV_MODULE_EVENT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0013)

/** 模块状态码（dev_subscribe_resp_t.current_state 取值） */
#define DEV_MODULE_STATE_NOT_RUNNING 0
#define DEV_MODULE_STATE_STARTING 1
#define DEV_MODULE_STATE_READY 2

/** 模块事件类型（dev_module_event_payload_t.event 取值） */
#define DEV_MODULE_EVENT_READY 0
#define DEV_MODULE_EVENT_DOWN 1

/** SUBSCRIBE 请求 payload（全部小端序，直接拷贝，未跨主机字节序敏感） */
typedef struct dev_subscribe_req
{
    uint32_t target_module_id; /**< 要订阅的模块 ID（网络字节序） */
    uint8_t auto_start;        /**< 1=未运行则触发启动；0=只订阅不拉起 */
    uint8_t _pad[3];
} dev_subscribe_req_t;

/** SUBSCRIBE 响应 payload */
typedef struct dev_subscribe_resp
{
    int32_t result;        /**< 0=成功；<0=失败错误码 */
    uint8_t current_state; /**< DEV_MODULE_STATE_* */
    uint8_t _pad[3];
    char host[64]; /**< 目标模块监听地址（仅 READY 时有效） */
    uint16_t port; /**< 目标模块监听端口（仅 READY 时有效，网络字节序） */
    uint16_t _pad2;
    uint32_t epoch; /**< 目标模块当前 epoch（网络字节序） */
} dev_subscribe_resp_t;

/** MODULE_EVENT 推送 payload */
typedef struct dev_module_event_payload
{
    uint32_t module_id; /**< 事件来源模块 ID（网络字节序） */
    uint8_t event;      /**< DEV_MODULE_EVENT_* */
    uint8_t _pad[3];
    char host[64]; /**< 模块监听地址 */
    uint16_t port; /**< 模块监听端口（网络字节序） */
    uint16_t _pad2;
    uint32_t epoch; /**< 本次事件对应的 epoch（网络字节序） */
} dev_module_event_payload_t;

// ============================================================================
// DB RPC 消息子类（大类 = DEV_IPC_CATEGORY_DB）
// ============================================================================

/** DB 执行 DML/DDL SQL（INSERT/UPDATE/DELETE/CREATE TABLE），返回影响行数或错误码 */
#define DEV_IPC_MSG_TYPE_DB_EXEC_SQL DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DB, 0x0001)
/** DB 执行 SELECT SQL，返回结果集 */
#define DEV_IPC_MSG_TYPE_DB_QUERY_SQL DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DB, 0x0002)
/** DB 操作响应 */
#define DEV_IPC_MSG_TYPE_DB_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DB, 0x0003)

// ============================================================================
// DEV IPC 公共 API
// ============================================================================

/**
 * @brief 创建 DEV IPC 消息
 * @param msg_type 消息类型 = (大类 << 16) | 子类
 * @param src_module_id 源模块 ID
 * @param dst_module_id 目标模块 ID
 * @param request_id 请求 ID
 * @param payload 负载数据
 * @param payload_len 负载长度
 * @param free_fn 负载释放函数
 * @return 新创建的消息，失败返回 NULL
 */
dev_ipc_message_t *dev_ipc_message_create(uint32_t msg_type, uint32_t src_module_id, uint32_t dst_module_id,
                                          uint32_t request_id, void *payload, size_t payload_len,
                                          void (*free_fn)(void *));

/**
 * @brief 释放 DEV IPC 消息
 * @param msg 待释放的消息
 */
void dev_ipc_message_free(dev_ipc_message_t *msg);

/**
 * @brief 初始化 DEV IPC 上下文
 * @param module_id   本模块 ID
 * @param name        模块名称
 * @param listen_port 本模块 IPC 监听端口（由调用方指定）
 * @param msg_handler 消息处理回调函数
 * @return IPC 上下文，失败返回 NULL
 */
dev_ipc_context_t *dev_ipc_init(uint32_t module_id, const char *name, uint16_t listen_port,
                                dev_ipc_msg_handler_fn msg_handler);

/**
 *  设置 IPC 连接断开回调
 *
 *  回调在 IPC IO 线程上下文执行，禁止阻塞、禁止调用 dev_ipc_query。
 *       如需访问业务状态，请投递到业务 worker 线程。
 */
void dev_ipc_set_disconnect_handler(dev_ipc_context_t *ctx, dev_ipc_disconnect_handler_fn handler, void *user);

/**
 * @brief 销毁 DEV IPC 上下文
 * @param ctx IPC 上下文
 */
void dev_ipc_destroy(dev_ipc_context_t *ctx);

/**
 * @brief 连接到目标模块
 * @param ctx              IPC 上下文
 * @param target_module_id 目标模块 ID
 * @param host             目标主机地址（如 "127.0.0.1"）
 * @param port             目标模块 IPC 监听端口
 * @return 成功返回 0，失败返回 -1
 */
int dev_ipc_connect(dev_ipc_context_t *ctx, uint32_t target_module_id, const char *host, uint16_t port);

/**
 * @brief 按 epoch 应用目标模块的权威 READY/DOWN 事件
 * @param ctx              IPC 上下文
 * @param target_module_id 目标模块 ID
 * @param epoch            目标进程 epoch
 * @param available        0=DOWN，1=READY
 * @return 事件被接受返回 1；旧 epoch 或同 epoch DOWN 后的 READY 返回 0
 *
 * DOWN 只暂停该目标主动连接的后续重连，不会立即关闭现有 socket，以便在途的
 * RESP_EXITING 等响应仍能被消费；READY 解除暂停并重置退避，由 dev_ipc_connect
 * 复用或重建连接。本接口可在 IPC IO 线程调用。
 */
int dev_ipc_apply_target_event(dev_ipc_context_t *ctx, uint32_t target_module_id, uint32_t epoch, int available);

/**
 * @brief 清空 IPC 上下文中的所有连接（用于 DEV 软件重启前重置连接状态）
 * @param ctx IPC 上下文
 */
void dev_ipc_clear_connections(dev_ipc_context_t *ctx);

/**
 * @brief 删除到指定模块的所有 IPC 连接记录
 * @param ctx              IPC 上下文
 * @param target_module_id 目标模块 ID
 *
 * 用于子进程退出后清理残留 conn：避免下次 spawn 时 dev_ipc_connect 命中
 * 旧 conn（已封顶到 RECONNECT_DELAY_MAX 的 backoff），导致新进程的 init 等待窗口
 * 与 IO 线程的重连计时器对齐失败。
 *
 * 实现安全性等同 dev_ipc_clear_connections：先停 IO 线程→拆 conn→重启 IO 线程，
 * 不可在 IO 线程上下文中调用。
 */
void dev_ipc_drop_connection(dev_ipc_context_t *ctx, uint32_t target_module_id);

/**
 * @brief 发送消息到目标模块（异步）
 * @param ctx IPC 上下文
 * @param target_module_id 目标模块 ID
 * @param msg 待发送的消息
 * @return 成功返回 0，失败返回 -1
 */
int dev_ipc_send(dev_ipc_context_t *ctx, uint32_t target_module_id, dev_ipc_message_t *msg);

/**
 * @brief 同步查询：发送请求并等待响应
 * @param ctx IPC 上下文
 * @param target_module_id 目标模块 ID
 * @param msg 请求消息
 * @param timeout_ms 超时时间（毫秒），0 使用默认值
 * @return 响应消息，超时或失败返回 NULL
 */
dev_ipc_message_t *dev_ipc_query(dev_ipc_context_t *ctx, uint32_t target_module_id, dev_ipc_message_t *msg,
                                 uint32_t timeout_ms);

/**
 * @brief 发送响应消息（根据 msg 中的 dst_module_id 路由）
 * @param ctx IPC 上下文
 * @param msg 响应消息
 * @return 成功返回 0，失败返回 -1
 */
int dev_ipc_send_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief 检查是否请求了关闭
 * @param ctx IPC 上下文
 * @return 已请求关闭返回非零值，否则返回 0
 */
int dev_ipc_shutdown_requested(dev_ipc_context_t *ctx);

/**
 * @brief 请求关闭
 * @param ctx IPC 上下文
 */
void dev_ipc_request_shutdown(dev_ipc_context_t *ctx);

/**
 * @brief 获取 IPC 上下文的模块 ID
 * @param ctx IPC 上下文
 * @return 模块 ID
 */
uint32_t dev_ipc_get_module_id(dev_ipc_context_t *ctx);

/**
 * @brief 检查是否已连接到目标模块
 * @param ctx IPC 上下文
 * @param target_module_id 目标模块 ID
 * @return 已连接返回 1，否则返回 0
 */
int dev_ipc_is_connected(dev_ipc_context_t *ctx, uint32_t target_module_id);

/**
 * @brief 获取本模块名称
 * @param ctx IPC 上下文
 * @return 本模块名称字符串（初始化时由调用方传入，生命周期与 ctx 相同）
 */
const char *dev_ipc_get_self_name(dev_ipc_context_t *ctx);

// ============================================================================
// 订阅 / 按需启动 API
// ============================================================================

/**
 * @brief 模块事件回调
 * @param module_id 事件来源模块 ID
 * @param event     DEV_MODULE_EVENT_READY / DEV_MODULE_EVENT_DOWN
 * @param host      模块监听地址（仅 READY 时有效）
 * @param port      模块监听端口（仅 READY 时有效）
 * @param epoch     模块当前 epoch（重启会递增）
 * @param user      订阅时传入的用户数据
 *
 * @note 回调在 IPC IO 线程上下文执行，禁止阻塞、禁止调用 dev_ipc_query。
 *       如需重活，请通过线程间消息队列投递到业务线程处理。
 */
typedef void (*dev_module_event_fn)(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                    void *user);

/**
 * @brief 订阅目标模块的就绪/下线事件
 *
 * 行为：
 *   - 向 DEV 发送 SUBSCRIBE RPC；DEV 内部把本模块加入 target 的订阅列表
 *   - 若 target 当前已 READY：DEV 响应即带 host/port，IPC 库自动 connect 并立即回调 READY
 *   - 若 target 未运行且 auto_start=1：DEV fork target；待 target 完成 NOTIFY_READY 后推送 MODULE_EVENT
 *
 * 同一 (ctx, target_module_id) 重复 subscribe 会覆盖旧的 callback/user。
 *
 * @param ctx        本模块 IPC 上下文
 * @param target_id  要订阅的模块 ID
 * @param auto_start 1=若 target 未运行则请求 DEV fork；0=只订阅
 * @param cb         事件回调（NULL 表示只用于建联，不需要业务感知）
 * @param user       回调用户数据
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int dev_ipc_subscribe_module(dev_ipc_context_t *ctx, uint32_t target_id, int auto_start, dev_module_event_fn cb,
                             void *user);

/**
 * @brief 取消订阅
 * @param ctx       本模块 IPC 上下文
 * @param target_id 要取消订阅的模块 ID
 * @return 成功返回 ERRCODE_SUCCESS
 */
int dev_ipc_unsubscribe_module(dev_ipc_context_t *ctx, uint32_t target_id);

/**
 * @brief 通知 DEV 本模块已完成本地初始化（含 deps 全连上 + DB restore 完成）
 *
 * DEV 收到后：phase=READY、epoch+=1、向所有 subscribers 推 MODULE_EVENT(READY)
 *
 * @param ctx 本模块 IPC 上下文
 * @return 成功返回 ERRCODE_SUCCESS
 */
int dev_ipc_notify_ready(dev_ipc_context_t *ctx);

/**
 * @brief 模块退出前通知 DEV 同步完成清理，收到 ACK 后再 exit()
 *
 * 流程：本模块 → DEV 发 DEV_IPC_MSG_TYPE_DEV_PRE_EXIT；DEV worker 同步完成
 *   phase=REGISTERED / broadcast DOWN / dev_ipc_drop_connection / m->pre_cleaned=1，
 *   然后回 ACK；本模块收到 ACK 后 destroy IPC 并 exit()。SIGCHLD 处理时见 pre_cleaned=1
 *   就跳过重复清理。
 *
 * 调用时机：在 dev_ipc_destroy() 之前，业务清理完毕（worker 已 shutdown，
 *   不再处理新业务消息）即可。
 *
 * @param ctx        本模块 IPC 上下文
 * @param timeout_ms ACK 等待超时（ms），建议 3000
 * @return 成功返回 ERRCODE_SUCCESS；超时/参数错返回 ERRCODE_FAIL
 *         （失败也应继续 exit；SIGCHLD 路径会兜底，仍能正确清理）
 */
int dev_ipc_pre_exit_notify(dev_ipc_context_t *ctx, uint32_t timeout_ms);

/**
 * @brief 阻塞等待已发起的 IPC 连接进入 CONNECTED 状态（轮询 is_connected）
 *
 * 适用于：本模块刚 dev_ipc_connect 到对端，需要等握手完成才能发首条 RPC（如 SUBSCRIBE）。
 *
 * @param ctx        本模块 IPC 上下文
 * @param target_id  要等待的对端模块 ID
 * @param timeout_ms 超时时间（毫秒）
 * @return 已连接返回 ERRCODE_SUCCESS，超时返回 ERRCODE_FAIL
 */
int dev_ipc_wait_connected(dev_ipc_context_t *ctx, uint32_t target_id, uint32_t timeout_ms);

/**
 * @brief wait_module_ready 进度回调
 *
 * @param target_id  目标模块 ID
 * @param state      DEV_MODULE_STATE_*
 * @param elapsed_ms 已等待时间（毫秒）
 * @param user       调用方上下文
 */
typedef void (*dev_ipc_wait_progress_fn)(uint32_t target_id, uint8_t state, uint32_t elapsed_ms, void *user);

/**
 * @brief 阻塞等待目标模块就绪并与之建立连接
 *
 * 内部用 SUBSCRIBE(auto_start=1) 触发 DEV 拉起按需模块；
 * 目标 init 完成后会主动 subscribe(CLI) 反向连接，本端 is_connected 变 true 即返回。
 *
 * 仅用于"需要 spawn"的场景（如 CFG 接收到配置命令）；read-only 命令不应调用此 API。
 *
 * @param ctx        本模块 IPC 上下文
 * @param target_id  要等待的模块 ID
 * @param timeout_ms 超时（毫秒，0=使用默认）
 * @return 成功（连接已建立）返回 ERRCODE_SUCCESS；超时/失败返回 ERRCODE_FAIL
 */
int dev_ipc_wait_module_ready(dev_ipc_context_t *ctx, uint32_t target_id, uint32_t timeout_ms);

/**
 * @brief 阻塞等待目标模块 READY，并在等待期间按状态/周期回调进度
 *
 * 语义同 dev_ipc_wait_module_ready()；progress_cb 可为 NULL。
 */
int dev_ipc_wait_module_ready_with_progress(dev_ipc_context_t *ctx, uint32_t target_id, uint32_t timeout_ms,
                                            dev_ipc_wait_progress_fn progress_cb, void *user);

/**
 * @brief 等待本模块自己订阅过的所有 peer 都进入 CONNECTED 状态
 *
 * 业务模块 init 序列推荐：
 *   1. dev_ipc_init + wait DEV connected
 *   2. subscribe(deps...)
 *   3. dev_ipc_wait_all_subscribed_connected()  ← 此函数，DEPS_READY 判定
 *   4. db_init + db_restore
 *   5. dev_ipc_notify_ready()                   ← 这时 CFG 才会派 config
 *
 * 内部 100ms 轮询 sub_mgr 中记录的所有订阅 target；不阻塞 IPC worker。
 *
 * @param ctx        本模块 IPC 上下文
 * @param timeout_ms 超时（毫秒，0=使用默认）
 * @return 全部连上返回 ERRCODE_SUCCESS；超时返回 ERRCODE_FAIL
 */
int dev_ipc_wait_all_subscribed_connected(dev_ipc_context_t *ctx, uint32_t timeout_ms);

/**
 * @brief 把本模块 sub_mgr 内的订阅条目格式化成 NUL 结尾文本（每行一个 subscription）
 *        供 show dev subscribe 远程拉取使用，调用方 g_free 返回的 buf。
 * @param ctx     本模块 IPC 上下文
 * @param out_len 返回 buf 长度（含 NUL）；NULL 不写
 * @return        NUL 结尾的字符串；ctx 无效返回 NULL
 */
char *dev_ipc_format_local_subs(dev_ipc_context_t *ctx, uint32_t *out_len);

// ============================================================================
// DEV IPC 内部 API（供 DEV IPC 子模块实现使用）
// ============================================================================

/**
 * @brief 创建连接对象
 * @param remote_module_id 对端模块 ID
 * @param is_initiator 是否为主动连接方
 * @return 新连接对象
 */
dev_ipc_connection_t *dev_ipc_connection_create(uint32_t remote_module_id, int is_initiator);

/**
 * @brief 销毁连接对象
 * @param conn 连接对象
 */
void dev_ipc_connection_destroy(dev_ipc_connection_t *conn);

/**
 * @brief 发起非阻塞 TCP 连接
 * @param conn 连接对象
 * @param host 目标地址
 * @param port 目标端口
 * @return 成功或正在连接返回 0，失败返回 -1
 */
int dev_ipc_connection_initiate(dev_ipc_connection_t *conn, const char *host, uint16_t port);

/**
 * @brief 关闭连接
 * @param conn 连接对象
 */
void dev_ipc_connection_close(dev_ipc_connection_t *conn);

/**
 * @brief 发送完整帧数据（阻塞写入）
 * @param conn 连接对象
 * @param data 数据
 * @param len 数据长度
 * @return 成功返回 0，失败返回 -1
 */
int dev_ipc_connection_send(dev_ipc_connection_t *conn, const uint8_t *data, uint32_t len);

/**
 * @brief 重置重连延迟
 * @param conn 连接对象
 */
void dev_ipc_connection_reset_reconnect(dev_ipc_connection_t *conn);

/**
 * @brief 增加重连延迟（指数退避）
 * @param conn 连接对象
 */
void dev_ipc_connection_backoff_reconnect(dev_ipc_connection_t *conn);

/**
 * @brief 初始化查询管理器
 * @return 查询管理器
 */
dev_ipc_query_mgr_t *dev_ipc_query_mgr_create(void);

/**
 * @brief 销毁查询管理器
 * @param mgr 查询管理器
 */
void dev_ipc_query_mgr_destroy(dev_ipc_query_mgr_t *mgr);

/**
 * @brief 分配新请求 ID 并注册挂起查询
 * @param mgr 查询管理器
 * @param target_module_id 目标模块 ID（用于连接断开时按目标取消）
 * @return 请求 ID
 */
uint32_t dev_ipc_query_mgr_register(dev_ipc_query_mgr_t *mgr, uint32_t target_module_id);

/**
 * @brief 等待查询响应
 * @param mgr 查询管理器
 * @param request_id 请求 ID
 * @param timeout_ms 超时时间（毫秒）
 * @return 响应消息，超时返回 NULL
 */
dev_ipc_message_t *dev_ipc_query_mgr_wait(dev_ipc_query_mgr_t *mgr, uint32_t request_id, uint32_t timeout_ms);

/**
 * @brief 完成挂起的查询（由 IO 线程调用）
 * @param mgr 查询管理器
 * @param request_id 请求 ID
 * @param response 响应消息（所有权转移给等待者）
 * @return 成功返回 0（找到对应挂起查询），未找到返回 -1
 */
int dev_ipc_query_mgr_complete(dev_ipc_query_mgr_t *mgr, uint32_t request_id, dev_ipc_message_t *response);

/**
 * @brief 取消挂起查询并清理状态（用于发送失败等场景）
 * @param mgr 查询管理器
 * @param request_id 请求 ID
 */
void dev_ipc_query_mgr_cancel(dev_ipc_query_mgr_t *mgr, uint32_t request_id);

/**
 * @brief 取消所有打到指定目标的挂起查询（连接断开时调用）
 *
 * 将匹配 target_module_id 的 pending query 置为 completed 且 response=NULL，
 * 唤醒等待者，让 dev_ipc_query() 立即返回 NULL 而不再等满超时。
 *
 * @param mgr 查询管理器
 * @param target_module_id 目标模块 ID
 */
void dev_ipc_query_mgr_cancel_by_target(dev_ipc_query_mgr_t *mgr, uint32_t target_module_id);

/**
 * @brief 将 dev_ipc_message_t 序列化为 IPC 帧（头部 + 负载）
 * @param msg 消息
 * @param out_buf 输出缓冲区指针（由函数分配，调用者 g_free）
 * @param out_len 输出总长度
 * @return 成功返回 0，失败返回 -1
 */
int dev_ipc_frame_serialize(const dev_ipc_message_t *msg, uint8_t **out_buf, uint32_t *out_len);

/**
 * @brief 从帧头部缓冲区解析头部
 * @param buf 24 字节帧头部
 * @param header 输出头部结构
 * @return 成功返回 0，失败返回 -1
 */
int dev_ipc_frame_parse_header(const uint8_t *buf, dev_ipc_message_t *header);

/**
 * @brief 从帧头部和负载重建 dev_ipc_message_t
 * @param header 已解析的帧头部
 * @param payload 负载数据（函数会拷贝）
 * @return 新创建的消息，失败返回 NULL
 */
dev_ipc_message_t *dev_ipc_frame_to_message(const dev_ipc_message_t *header, const uint8_t *payload);

/**
 * @brief 创建订阅管理器（由 dev_ipc_init 调用）
 */
dev_ipc_subscribe_mgr_t *dev_ipc_subscribe_mgr_create(void);

/**
 * @brief 销毁订阅管理器（由 dev_ipc_destroy 调用）
 */
void dev_ipc_subscribe_mgr_destroy(dev_ipc_subscribe_mgr_t *mgr);

/**
 * @brief IO 线程：路由 DEV_IPC_MSG_TYPE_DEV_MODULE_EVENT 帧到本地订阅回调
 * @param ctx IPC 上下文
 * @param pl  事件 payload
 */
void dev_ipc_dispatch_module_event(dev_ipc_context_t *ctx, const dev_module_event_payload_t *pl);

/**
 * @brief 序列化本模块所有 IPC 连接状态为 QUERY_IPC_CONNS wire format 二进制载荷
 *
 * 载荷格式（全部 big-endian）：
 *   uint32_t  num_connections
 *   Per entry (100 bytes each):
 *     uint32_t  remote_module_id
 *     uint32_t  state
 *     uint32_t  is_initiator
 *     char      remote_host[64]
 *     uint16_t  remote_port
 *     uint8_t   _pad[2]
 *     uint32_t  last_hb_sent_hi/lo
 *     uint32_t  last_hb_recv_hi/lo
 *     uint32_t  reconnect_delay_ms
 *
 * @param ctx     IPC 上下文
 * @param out_len 输出载荷长度
 * @return 序列化后的载荷（调用者负责 g_free），失败返回 NULL
 */
uint8_t *dev_ipc_build_conns_payload(dev_ipc_context_t *ctx, uint32_t *out_len);

// ============================================================================
// DEV RPC API
// ============================================================================

/**
 * @brief 通过 DEV RPC 根据模块 ID 获取模块名称
 * @param ctx 调用方 IPC 上下文
 * @param module_id 模块 ID
 * @param module_name 输出模块名称缓冲区
 * @return 成功返回 0，失败返回 -1
 */
int dev_get_module_name(dev_ipc_context_t *ctx, uint32_t module_id, char *module_name);

#endif // DEV_H
