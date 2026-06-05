/**
 * @file   route.h
 * @brief  Route 模块公共接口：协议常量、消息类型、IPC 载荷结构
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_H
#define ROUTE_H

#include <stdint.h>

#include "dev.h"

// ============================================================================
// 路由协议常量
// ============================================================================

/** 直连路由协议 */
#define ROUTE_PROTOCOL_CONNECTED 0u
/** 静态路由协议 */
#define ROUTE_PROTOCOL_STATIC 1u
/** BGP 路由协议 */
#define ROUTE_PROTOCOL_BGP 2u
/** OSPF 路由协议 */
#define ROUTE_PROTOCOL_OSPF 3u
/** 黑洞路由（null0 接口，静默丢弃） */
#define ROUTE_PROTOCOL_BLACKHOLE 4u
/** ISIS 路由协议 */
#define ROUTE_PROTOCOL_ISIS 5u
/** 通配符：匹配所有协议（用于订阅/查询过滤） */
#define ROUTE_PROTOCOL_MAX 0xFFFFFFFFu

// ============================================================================
// VRF 常量
// ============================================================================

/** 默认 VRF ID */
#define ROUTE_VRF_DEFAULT 0u
/** 通配符：匹配所有 VRF（用于订阅/查询过滤） */
#define ROUTE_VRF_ALL 0xFFFFFFFFu

// ============================================================================
// AFI/SAFI 常量（与 BGP 保持一致）
// ============================================================================

/** IPv4 地址族 */
#define ROUTE_AFI_IPV4 1u
/** IPv6 地址族 */
#define ROUTE_AFI_IPV6 2u
/** 通配符：匹配所有地址族（用于订阅/查询过滤） */
#define ROUTE_AFI_ALL 0xFFFFu
/** 单播子地址族 */
#define ROUTE_SAFI_UNICAST 1u

/** 普通 IP 下一跳 */
#define ROUTE_NH_TYPE_IP 1u
/** 隧道下一跳（通过 tunnel_id 关联完整隧道信息） */
#define ROUTE_NH_TYPE_TUNNEL 2u
/** 黑洞下一跳 */
#define ROUTE_NH_TYPE_BLACKHOLE 3u

// ============================================================================
// 管理距离（Administrative Distance）
// ============================================================================

/** 直连路由管理距离 */
#define ROUTE_ADMIN_DIST_CONNECTED 0
/** 静态路由管理距离 */
#define ROUTE_ADMIN_DIST_STATIC 1
/** OSPF 路由管理距离 */
#define ROUTE_ADMIN_DIST_OSPF 110
/** ISIS 路由管理距离 */
#define ROUTE_ADMIN_DIST_ISIS 115
/** BGP 路由管理距离 */
#define ROUTE_ADMIN_DIST_BGP 200

// ============================================================================
// IPC 消息类型
// ============================================================================

/** 订阅路由更新 */
#define ROUTE_MSG_TYPE_SUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0001)
/** 取消订阅 */
#define ROUTE_MSG_TYPE_UNSUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0002)
/** 全量路由上报（响应 SUBSCRIBE+FULL 标志） */
#define ROUTE_MSG_TYPE_REPORT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0003)
/** 增量路由更新（单条路由新增/撤销） */
#define ROUTE_MSG_TYPE_UPDATE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0004)
/** 路由注入请求（其他模块 -> ROUTE，payload=route_msg_entry_t） */
#define ROUTE_MSG_TYPE_INJECT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0010)
/** nexthop 迭代注册（其他模块 -> ROUTE，payload=route_nh_iter_req_t） */
#define ROUTE_MSG_TYPE_NH_REGISTER DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0011)
/** nexthop 迭代取消注册（其他模块 -> ROUTE，payload=route_nh_iter_req_t） */
#define ROUTE_MSG_TYPE_NH_UNREGISTER DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0012)
/** nexthop 迭代状态通知（ROUTE -> owner 模块，payload=route_nh_iter_notify_t） */
#define ROUTE_MSG_TYPE_NH_NOTIFY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0013)
/** nexthop 对象申请（其他模块 -> ROUTE，payload=route_nhobj_msg_t，ACK 返回 nexthop_id） */
#define ROUTE_MSG_TYPE_NHOBJ_ACQUIRE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0014)
/** nexthop 对象释放（其他模块 -> ROUTE，payload=route_nhobj_release_req_t） */
#define ROUTE_MSG_TYPE_NHOBJ_RELEASE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x0015)
/** 通用应答 */
#define ROUTE_MSG_TYPE_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0x00FF)

// ============================================================================
// 订阅标志
// ============================================================================

/** 订阅时请求全量路由快照 */
#define ROUTE_SUBSCRIBE_FLAG_FULL (1u << 0)

// ============================================================================
// 路由条目标志位（route_msg_entry_t.flags / route_path_t.flags 高位区共享语义位）
// ============================================================================

/** 不允许对外发布（被引入到协议时不参与对外通告） */
#define ROUTE_ENTRY_FLAG_NO_ADV (1u << 0)

// ============================================================================
// IPC 载荷结构
// ============================================================================

#include "net_addr.h"

/** nexthop 对象去重键；协议侧和 ROUTE 必须用同一 key 生成 nexthop_id */
typedef struct route_nhobj_key
{
    uint32_t vrf_id;      /**< VRF ID */
    uint32_t protocol;    /**< 申请该 nexthop 的路由协议（ROUTE_PROTOCOL_*） */
    uint16_t afi;         /**< 地址族 */
    uint8_t nh_type;      /**< ROUTE_NH_TYPE_IP / ROUTE_NH_TYPE_BLACKHOLE */
    uint8_t _pad0;        /**< 对齐填充 */
    uint32_t key_ifindex; /**< 明确参与 nexthop 身份的接口索引，0=不按接口区分 */
    net_addr_t nexthop;   /**< 原始下一跳地址（blackhole/interface-only 为空地址） */
} route_nhobj_key_t;

/**
 * nexthop 对象 id 按协议分区：ROUTE 在 [protocol*SPAN+1, (protocol+1)*SPAN+1) 区间内分配。
 * 不同协议的 id 永不重叠，业务进程重启后用原 id 反刷恢复（落在自己分区，不与 ROUTE 期间
 * 新分配的其它协议 id 冲突）。id 始终由 ROUTE 分配；业务侧只在重启后用记住的原 id 请求恢复。
 */
#define ROUTE_NHOBJ_ID_SPAN 1000000u
/** 取某协议 nexthop id 分区的起始值（含） */
#define ROUTE_NHOBJ_ID_BASE(protocol) ((uint32_t)(protocol) * ROUTE_NHOBJ_ID_SPAN + 1u)

/**
 * @brief 订阅请求载荷
 */
typedef struct route_subscribe_req
{
    uint32_t protocol; /**< 订阅的路由协议（ROUTE_PROTOCOL_MAX = 全部） */
    uint32_t vrf_id;   /**< 订阅的 VRF ID（ROUTE_VRF_ALL = 全部） */
    uint16_t afi;      /**< 订阅的地址族（ROUTE_AFI_ALL = 全部） */
    uint16_t _pad;     /**< 对齐填充 */
    uint32_t flags;    /**< 标志位（ROUTE_SUBSCRIBE_FLAG_FULL 等） */
} route_subscribe_req_t;

/**
 * @brief 单条路由条目（用于 REPORT/UPDATE 消息）
 */
typedef struct route_msg_entry
{
    uint32_t vrf_id;      /**< VRF ID */
    uint16_t afi;         /**< 地址族 */
    uint8_t safi;         /**< 子地址族 */
    uint8_t prefix_len;   /**< 前缀长度 */
    uint32_t protocol;    /**< 路由协议 */
    int32_t metric;       /**< 度量值 */
    int32_t preference;   /**< 管理距离（偏好值） */
    uint8_t is_withdraw;  /**< 1=撤销路由, 0=新增/更新路由 */
    uint8_t flags;        /**< 路由条目标志位（ROUTE_ENTRY_FLAG_*） */
    uint8_t nh_type;      /**< ROUTE_NH_TYPE_* */
    uint8_t _pad;         /**< 对齐填充 */
    uint32_t tunnel_id;   /**< nh_type=ROUTE_NH_TYPE_TUNNEL 时的隧道 ID */
    uint32_t nexthop_id;  /**< nh_type=IP/BLACKHOLE 时引用的 nexthop 对象 ID（0=按旧 value 字段生成） */
    uint32_t out_ifindex; /**< 原始出接口索引（由发布方携带，0=不指定） */
    uint32_t iter_out_ifindex;    /**< 迭代解析后的出接口索引（0=未知） */
    net_addr_t prefix_addr;       /**< 前缀地址（二进制） */
    net_addr_t nexthop_addr;      /**< 原始下一跳地址（二进制） */
    net_addr_t iter_nexthop_addr; /**< 迭代解析后的下一跳地址（二进制，family=0 表示未知） */
    net_addr_t source_addr;       /**< 路径来源标识（二进制 IP） */
} route_msg_entry_t;

/**
 * @brief 全量路由上报载荷（ROUTE_MSG_TYPE_REPORT）
 */
typedef struct route_msg_report
{
    uint32_t protocol;          /**< 上报的路由协议 */
    uint32_t route_count;       /**< 路由条目数量 */
    route_msg_entry_t routes[]; /**< 路由条目数组（flexible array member） */
} route_msg_report_t;

/**
 * @brief 通用应答载荷（ROUTE_MSG_TYPE_ACK）
 */
typedef struct route_msg_ack
{
    int32_t result;      /**< 错误码（ERRCODE_SUCCESS = 0 表示成功） */
    uint32_t nexthop_id; /**< ROUTE_MSG_TYPE_NHOBJ_ACQUIRE 成功时返回的 nexthop ID */
} route_msg_ack_t;

/**
 * @brief nexthop 对象申请载荷
 */
typedef struct route_nhobj_msg
{
    route_nhobj_key_t key;  /**< nexthop 身份键 */
    net_addr_t relay_addr;  /**< relay/网关地址；无 relay 时传 key.nexthop 或全零 */
    uint32_t relay_ifindex; /**< relay 出接口 */
    uint32_t nexthop_id;    /**< 期望 id：0=ROUTE 自行分配；非 0=ROUTE 进程重启后按该 id 恢复对象 */
} route_nhobj_msg_t;

/**
 * @brief nexthop 对象释放载荷
 */
typedef struct route_nhobj_release_req
{
    uint32_t nexthop_id; /**< route_rpc_nhobj_acquire_wait 返回的 ID */
} route_nhobj_release_req_t;

/**
 * @brief nexthop 迭代注册/取消注册请求
 */
typedef struct route_nh_iter_req
{
    uint32_t nexthop_id; /**< 已申请的 nexthop 对象 ID */
    uint8_t safi;        /**< 子地址族（当前仅支持单播；0=默认单播） */
    uint8_t _pad0[3];    /**< 对齐填充 */
} route_nh_iter_req_t;

/**
 * @brief nexthop 迭代状态通知
 */
typedef struct route_nh_iter_notify
{
    uint32_t nexthop_id;   /**< nexthop 对象 ID */
    uint32_t vrf_id;       /**< VRF ID */
    uint16_t afi;          /**< 地址族 */
    uint8_t safi;          /**< 子地址族 */
    uint8_t resolved;      /**< 1=可达，0=不可达 */
    uint32_t out_ifindex;  /**< 解析出的出接口索引（0=不可达或未知） */
    net_addr_t relay_addr; /**< 迭代后的 relay 地址（二进制） */
} route_nh_iter_notify_t;

// ============================================================================
// ROUTE 注入 API（供其他模块调用）
// ============================================================================

/**
 * @brief 通过 IPC 向 ROUTE 模块注入一条路径（add/update 或 withdraw）
 * @param ctx   调用方 IPC 上下文（需已连接到 ROUTE 模块）
 * @param entry 路由消息条目
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int route_rpc_add(dev_ipc_context_t *ctx, const route_msg_entry_t *entry);

/**
 * @brief 同步注入一条路径：等待 ROUTE 模块处理完成并返回 ACK
 * @param ctx        调用方 IPC 上下文
 * @param entry      路由消息条目
 * @param timeout_ms 等待超时（毫秒，0 表示默认）
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int route_rpc_add_wait(dev_ipc_context_t *ctx, const route_msg_entry_t *entry, uint32_t timeout_ms);

/**
 * @brief 通过 IPC 向 ROUTE 模块注册 nexthop 迭代监听
 * @param ctx 调用方 IPC 上下文
 * @param req 迭代请求（nexthop_id, safi）
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int route_rpc_nh_register(dev_ipc_context_t *ctx, const route_nh_iter_req_t *req);

/**
 * @brief 通过 IPC 向 ROUTE 申请/引用一个 nexthop 对象，并同步返回 nexthop_id
 */
int route_rpc_nhobj_acquire_wait(dev_ipc_context_t *ctx, const route_nhobj_msg_t *req, uint32_t timeout_ms,
                                 uint32_t *nexthop_id_out);

/**
 * @brief 通过 IPC 释放一个 nexthop 对象引用
 */
int route_rpc_nhobj_release(dev_ipc_context_t *ctx, uint32_t nexthop_id);

/**
 * @brief 通过 IPC 向 ROUTE 模块撤销一条路径
 * @param ctx   调用方 IPC 上下文
 * @param entry 路由消息条目（该 API 会强制按 withdraw 语义发送）
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int route_rpc_del(dev_ipc_context_t *ctx, const route_msg_entry_t *entry);

/**
 * @brief 同步撤销一条路径：等待 ROUTE 模块处理完成并返回 ACK
 * @param ctx        调用方 IPC 上下文
 * @param entry      路由消息条目（该 API 会强制按 withdraw 语义发送）
 * @param timeout_ms 等待超时（毫秒，0 表示默认）
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int route_rpc_del_wait(dev_ipc_context_t *ctx, const route_msg_entry_t *entry, uint32_t timeout_ms);

/**
 * @brief 通过 IPC 取消 nexthop 迭代监听
 * @param ctx 调用方 IPC 上下文
 * @param req 迭代请求（nexthop_id, safi）
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int route_rpc_nh_unregister(dev_ipc_context_t *ctx, const route_nh_iter_req_t *req);

#endif /* ROUTE_H */
