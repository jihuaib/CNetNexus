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
#define DEV_MODULE_ID_CFG 0x00000003
/** IF 接口模块 */
#define DEV_MODULE_ID_IF 0x00000004
/** BGP 模块 */
#define DEV_MODULE_ID_BGP 0x00000005
/** Route 模块 */
#define DEV_MODULE_ID_ROUTE 0x00000006
/** VRF 模块 */
#define DEV_MODULE_ID_VRF 0x00000007

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
#define DEV_MODULE_PORT_CFG 4003
/** IF 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_IF 4004
/** BGP 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_BGP 4005
/** ROUTE 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_ROUTE 4006
/** VRF 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_VRF 4007

// ============================================================================
// IPC 前向声明
// ============================================================================

typedef struct dev_ipc_context dev_ipc_context_t;
typedef struct dev_ipc_context dev_ipc_context_t;

typedef struct dev_ipc_message dev_ipc_message_t;
typedef struct dev_ipc_message dev_ipc_message_t;

/**
 * @brief DEV IPC 消息处理回调函数类型
 * @param ctx IPC 上下文
 * @param msg 接收到的消息
 */
typedef void (*dev_ipc_msg_handler_fn)(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
typedef dev_ipc_msg_handler_fn dev_ipc_msg_handler_fn;

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

// ============================================================================
// DEV IPC 消息结构
// ============================================================================

/** DEV IPC 魔数 "NNIP" */
#define DEV_IPC_MAGIC 0x4E4E4950

/** DEV IPC 帧头部大小（网络传输时只序列化前 24 字节） */
#define DEV_IPC_FRAME_HEADER_SIZE 24

/**
 * @brief 统一 DEV IPC 消息结构
 *
 * 前 24 字节（magic 到 payload_len）用于网络传输，
 * payload 和 free_fn 仅在内存中使用。
 */
struct dev_ipc_message
{
    uint32_t magic;          /**< 魔数 0x4E4E4950 ("NNIP") */
    uint32_t msg_type;       /**< 消息类型 = (大类 << 16) | 子类 */
    uint32_t src_module_id;  /**< 源模块 ID */
    uint32_t dst_module_id;  /**< 目标模块 ID */
    uint32_t request_id;     /**< 请求 ID，用于请求/响应配对 */
    uint32_t payload_len;    /**< 负载长度 */
    void *payload;           /**< 负载数据（内存中使用） */
    void (*free_fn)(void *); /**< 负载释放函数（内存中使用） */
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

/** 心跳间隔（秒） */
#define DEV_IPC_HEARTBEAT_INTERVAL 5
/** 心跳超时（秒） */
#define DEV_IPC_HEARTBEAT_TIMEOUT 15
/** 初始重连延迟（毫秒） */
#define DEV_IPC_RECONNECT_DELAY_MIN 500
/** 最大重连延迟（毫秒） */
#define DEV_IPC_RECONNECT_DELAY_MAX 10000
/** 模块名称最大长度 */
#define DEV_IPC_MODULE_NAME_MAX 32
/** 最大连接数 */
#define DEV_IPC_MAX_CONNECTIONS 16
/** 同步查询默认超时（毫秒） */
#define DEV_IPC_QUERY_TIMEOUT_DEFAULT 5000
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

    /* 是否为主动发起方 */
    int is_initiator; /**< 1=主动连接方，0=被接受方 */
} dev_ipc_connection_t;

typedef dev_ipc_connection_t dev_ipc_connection_t;

/** 挂起的同步查询 */
typedef struct dev_ipc_pending_query
{
    uint32_t request_id;         /**< 请求 ID */
    dev_ipc_message_t *response; /**< 响应消息（由 IO 线程设置） */
    pthread_mutex_t mutex;       /**< 互斥锁 */
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

/** IPC 上下文完整定义 */
struct dev_ipc_context
{
    /* 本模块信息 */
    uint32_t module_id;                 /**< 本模块 ID */
    char name[DEV_IPC_MODULE_NAME_MAX]; /**< 模块名称 */

    /* 消息处理 */
    dev_ipc_msg_handler_fn msg_handler; /**< 消息处理回调 */

    /* 连接 */
    dev_ipc_connection_t *connections[DEV_IPC_MAX_CONNECTIONS]; /**< 连接数组 */
    int num_connections;                                        /**< 连接数 */
    pthread_mutex_t comutex;                                    /**< 连接锁 */

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

/** 模块启动通知（Phase 1） */
#define DEV_IPC_MSG_TYPE_DEV_MODULE_START DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0001)
/** 模块建连通知（Phase 2） */
#define DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0002)
/** 模块就绪通知（Phase 3） */
#define DEV_IPC_MSG_TYPE_DEV_MODULE_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0003)
/** 模块关闭通知 */
#define DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0004)
/** 查询模块名称 */
#define DEV_IPC_MSG_TYPE_DEV_GET_MODULE_NAME DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x0005)
/** 模块阶段响应 */
#define DEV_IPC_MSG_TYPE_DEV_MODULE_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_DEV, 0x000F)

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
 * @return 请求 ID
 */
uint32_t dev_ipc_query_mgr_register(dev_ipc_query_mgr_t *mgr);

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
