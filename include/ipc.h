/**
 * @file   ipc.h
 * @brief  IPC 模块公共接口，提供跨进程 TCP 通信 API
 * @author jhb
 * @date   2026/02/02
 */

#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <stdint.h>

#include "dev.h" /* 模块 ID 定义 */

// ============================================================================
// 全局变量
// ============================================================================

typedef struct ipc_context ipc_context_t; // Forward declaration needed here or move down
extern ipc_context_t *g_ipc_context;

// ============================================================================
// 消息类型编码：msg_type = (大类 << 16) | 子类
// ============================================================================

/** 提取消息大类（高 16 位） */
#define IPC_MSG_CATEGORY(t) (((t) >> 16) & 0xFFFF)
/** 提取消息子类（低 16 位） */
#define IPC_MSG_SUBTYPE(t) ((t) & 0xFFFF)
/** 由大类和子类构造 msg_type */
#define IPC_MSG_TYPE(cat, sub) (((uint32_t)(cat) << 16) | ((uint32_t)(sub) & 0xFFFF))

// ============================================================================
// 消息大类定义（与模块 ID 对齐）
// ============================================================================

/** IPC 内部消息大类 */
#define IPC_CATEGORY_INTERNAL 0x0000
/** DEV 模块消息大类 */
#define IPC_CATEGORY_DEV 0x0001
/** DB 模块消息大类 */
#define IPC_CATEGORY_DB 0x0002
/** CLI 模块消息大类 */
#define IPC_CATEGORY_CLI 0x0003
/** IF 模块消息大类 */
#define IPC_CATEGORY_IF 0x0004
/** BGP 模块消息大类 */
#define IPC_CATEGORY_BGP 0x0005
/** ROUTE 模块消息大类 */
#define IPC_CATEGORY_ROUTE 0x0006

// ============================================================================
// IPC 消息结构
// ============================================================================

/** IPC 魔数 "NNIP" */
#define IPC_MAGIC 0x4E4E4950

/** IPC 帧头部大小（网络传输时只序列化前 24 字节） */
#define IPC_FRAME_HEADER_SIZE 24

/**
 * @brief 统一 IPC 消息结构
 *
 * 前 24 字节（magic 到 payload_len）用于网络传输，
 * payload 和 free_fn 仅在内存中使用。
 */
typedef struct ipc_message
{
    uint32_t magic;          /**< 魔数 0x4E4E4950 ("NNIP") */
    uint32_t msg_type;       /**< 消息类型 = (大类 << 16) | 子类 */
    uint32_t src_module_id;  /**< 源模块 ID */
    uint32_t dst_module_id;  /**< 目标模块 ID */
    uint32_t request_id;     /**< 请求 ID，用于请求/响应配对 */
    uint32_t payload_len;    /**< 负载长度 */
    void *payload;           /**< 负载数据（内存中使用） */
    void (*free_fn)(void *); /**< 负载释放函数（内存中使用） */
} ipc_message_t;

/**
 * @brief 创建 IPC 消息
 * @param msg_type 消息类型 = (大类 << 16) | 子类
 * @param src_module_id 源模块 ID
 * @param dst_module_id 目标模块 ID
 * @param request_id 请求 ID
 * @param payload 负载数据
 * @param payload_len 负载长度
 * @param free_fn 负载释放函数
 * @return 新创建的消息，失败返回 NULL
 */
ipc_message_t *ipc_message_create(uint32_t msg_type, uint32_t src_module_id, uint32_t dst_module_id,
                                  uint32_t request_id, void *payload, size_t payload_len, void (*free_fn)(void *));

/**
 * @brief 释放 IPC 消息
 * @param msg 待释放的消息
 */
void ipc_message_free(ipc_message_t *msg);

// ============================================================================
// IPC 内部消息子类（大类 = IPC_CATEGORY_INTERNAL）
// ============================================================================

/** 握手请求 */
#define IPC_MSG_TYPE_HANDSHAKE IPC_MSG_TYPE(IPC_CATEGORY_INTERNAL, 0x0001)
/** 握手响应 */
#define IPC_MSG_TYPE_HANDSHAKE_ACK IPC_MSG_TYPE(IPC_CATEGORY_INTERNAL, 0x0002)
/** 心跳请求 */
#define IPC_MSG_TYPE_HEARTBEAT IPC_MSG_TYPE(IPC_CATEGORY_INTERNAL, 0x0003)
/** 心跳响应 */
#define IPC_MSG_TYPE_HEARTBEAT_ACK IPC_MSG_TYPE(IPC_CATEGORY_INTERNAL, 0x0004)
/** 关闭通知 */
#define IPC_MSG_TYPE_SHUTDOWN IPC_MSG_TYPE(IPC_CATEGORY_INTERNAL, 0x0005)

// ============================================================================
// DEV 模块生命周期消息子类（大类 = IPC_CATEGORY_DEV）
// ============================================================================

/** 模块启动通知（Phase 1） */
#define IPC_MSG_TYPE_DEV_MODULE_START IPC_MSG_TYPE(IPC_CATEGORY_DEV, 0x0001)
/** 模块建连通知（Phase 2） */
#define IPC_MSG_TYPE_DEV_MODULE_CONNECT IPC_MSG_TYPE(IPC_CATEGORY_DEV, 0x0002)
/** 模块就绪通知（Phase 3） */
#define IPC_MSG_TYPE_DEV_MODULE_READY IPC_MSG_TYPE(IPC_CATEGORY_DEV, 0x0003)
/** 模块关闭通知 */
#define IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN IPC_MSG_TYPE(IPC_CATEGORY_DEV, 0x0004)
/** 模块阶段响应 */
#define IPC_MSG_TYPE_DEV_MODULE_RESP IPC_MSG_TYPE(IPC_CATEGORY_DEV, 0x000F)

// ============================================================================
// DB RPC 消息子类（大类 = IPC_CATEGORY_DB）
// ============================================================================

/** DB INSERT 操作 */
#define IPC_MSG_TYPE_DB_INSERT IPC_MSG_TYPE(IPC_CATEGORY_DB, 0x0001)
/** DB UPDATE 操作 */
#define IPC_MSG_TYPE_DB_UPDATE IPC_MSG_TYPE(IPC_CATEGORY_DB, 0x0002)
/** DB DELETE 操作 */
#define IPC_MSG_TYPE_DB_DELETE IPC_MSG_TYPE(IPC_CATEGORY_DB, 0x0003)
/** DB QUERY 操作 */
#define IPC_MSG_TYPE_DB_QUERY IPC_MSG_TYPE(IPC_CATEGORY_DB, 0x0004)
/** DB EXISTS 操作 */
#define IPC_MSG_TYPE_DB_EXISTS IPC_MSG_TYPE(IPC_CATEGORY_DB, 0x0005)
/** DB 操作响应 */
#define IPC_MSG_TYPE_DB_RESP IPC_MSG_TYPE(IPC_CATEGORY_DB, 0x0006)
/** DB Registry 注册请求 */
#define IPC_MSG_TYPE_DB_REGISTRY_ADD IPC_MSG_TYPE(IPC_CATEGORY_DB, 0x0007)

// ============================================================================
// IPC 连接状态
// ============================================================================

/** 连接状态枚举 */
typedef enum ipc_costate
{
    IPC_CODISCONNECTED = 0, /**< 未连接 */
    IPC_COCONNECTING,       /**< 连接中 */
    IPC_COHANDSHAKING,      /**< 握手中 */
    IPC_COCONNECTED,        /**< 已连接 */
    IPC_CORECONNECTING      /**< 重连中 */
} ipc_costate_t;

// ============================================================================
// IPC 配置
// ============================================================================

/** 心跳间隔（秒） */
#define IPC_HEARTBEAT_INTERVAL 5
/** 心跳超时（秒） */
#define IPC_HEARTBEAT_TIMEOUT 15
/** 初始重连延迟（毫秒） */
#define IPC_RECONNECT_DELAY_MIN 500
/** 最大重连延迟（毫秒） */
#define IPC_RECONNECT_DELAY_MAX 10000
/** 模块名称最大长度 */
#define IPC_MODULE_NAME_MAX 32
/** 最大连接数 */
#define IPC_MAX_CONNECTIONS 16
/** 同步查询默认超时（毫秒） */
#define IPC_QUERY_TIMEOUT_DEFAULT 5000
/** 接收缓冲区大小 */
#define IPC_RECV_BUF_SIZE 65536

// ============================================================================
// 核心 API
// ============================================================================

/**
 * @brief 初始化 IPC 上下文
 * @param module_id 本模块 ID
 * @param name 模块名称
 * @param config_path IPC 配置文件路径（可为 NULL，使用默认路径）
 * @param msg_handler 消息处理回调函数
 * @return IPC 上下文，失败返回 NULL
 */
ipc_context_t *ipc_init(uint32_t module_id, const char *name, const char *config_path, ipc_msg_handler_fn msg_handler);

/**
 * @brief 销毁 IPC 上下文
 * @param ctx IPC 上下文
 */
void ipc_destroy(ipc_context_t *ctx);

/**
 * @brief 连接到目标模块
 * @param ctx IPC 上下文
 * @param target_module_id 目标模块 ID
 * @return 成功返回 0，失败返回 -1
 */
int ipc_connect(ipc_context_t *ctx, uint32_t target_module_id);

/**
 * @brief 发送消息到目标模块（异步）
 * @param ctx IPC 上下文
 * @param target_module_id 目标模块 ID
 * @param msg 待发送的消息
 * @return 成功返回 0，失败返回 -1
 */
int ipc_send(ipc_context_t *ctx, uint32_t target_module_id, ipc_message_t *msg);

/**
 * @brief 同步查询：发送请求并等待响应
 * @param ctx IPC 上下文
 * @param target_module_id 目标模块 ID
 * @param msg 请求消息
 * @param timeout_ms 超时时间（毫秒），0 使用默认值
 * @return 响应消息，超时或失败返回 NULL
 */
ipc_message_t *ipc_query(ipc_context_t *ctx, uint32_t target_module_id, ipc_message_t *msg, uint32_t timeout_ms);

/**
 * @brief 发送响应消息（根据 msg 中的 sender_id 路由）
 * @param ctx IPC 上下文
 * @param msg 响应消息（msg->sender_id 必须设置为原始请求的 sender_id）
 * @return 成功返回 0，失败返回 -1
 */
int ipc_send_response(ipc_context_t *ctx, ipc_message_t *msg);

/**
 * @brief 检查是否请求了关闭
 * @param ctx IPC 上下文
 * @return 已请求关闭返回非零值，否则返回 0
 */
int ipc_shutdown_requested(ipc_context_t *ctx);

/**
 * @brief 请求关闭
 * @param ctx IPC 上下文
 */
void ipc_request_shutdown(ipc_context_t *ctx);

/**
 * @brief 获取模块名称
 * @param module_id 模块 ID
 * @return 模块名称字符串，未找到返回 "unknown"
 */
const char *ipc_get_module_name(uint32_t module_id);

/**
 * @brief 获取 IPC 上下文的模块 ID
 * @param ctx IPC 上下文
 * @return 模块 ID
 */
uint32_t ipc_get_module_id(ipc_context_t *ctx);

/**
 * @brief 检查是否已连接到目标模块
 * @param ctx IPC 上下文
 * @param target_module_id 目标模块 ID
 * @return 已连接返回 1，否则返回 0
 */
int ipc_is_connected(ipc_context_t *ctx, uint32_t target_module_id);

#endif // IPC_H
