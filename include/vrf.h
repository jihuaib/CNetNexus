/**
 * @file   vrf.h
 * @brief  VRF 模块公共接口：常量、类型、IPC 协议、RPC 与 lib API
 * @author jhb
 * @date   2026/03/05
 */
#ifndef VRF_H
#define VRF_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

#include "dev.h"

// ============================================================================
// 常量
// ============================================================================

/** VRF 名称最大长度（含 null 终止符） */
#define VRF_NAME_MAX_LEN 64

/** 公网 VRF ID（系统启动时自动创建，不可删除） */
#define VRF_PUBLIC_VRF_ID 0

/** 公网 VRF 名称 */
#define VRF_PUBLIC_VRF_NAME "public"

/** OS L3VRF 路由表 ID 起始基址（避开 main/local/default） */
#define VRF_OS_TABLE_BASE 1000u

/** VRF OS 状态：未知 */
#define VRF_OS_STATE_UNKNOWN 0
/** VRF OS 状态：UP */
#define VRF_OS_STATE_UP 1
/** VRF OS 状态：DOWN */
#define VRF_OS_STATE_DOWN 2

/** AFI：IPv4（与 ROUTE_AFI_IPV4 / BGP_AFI_IPV4 数值一致） */
#define VRF_AFI_IPV4 1u
/** AFI：IPv6 */
#define VRF_AFI_IPV6 2u

/** SAFI：单播 */
#define VRF_SAFI_UNICAST 1u

// ============================================================================
// 数据类型
// ============================================================================

/**
 * @brief Route Distinguisher（8 字节，RFC 4364，大端原始字节）
 */
typedef struct vrf_rd
{
    uint8_t bytes[8]; /**< 原始字节 */
} vrf_rd_t;

/**
 * @brief Route Target（8 字节，RFC 4364 扩展团体属性）
 */
typedef struct vrf_rt
{
    uint8_t bytes[8]; /**< 原始字节 */
} vrf_rt_t;

/**
 * @brief AF 维度的 RD/RT 配置（lib 缓存视图）
 *
 * import_rts / export_rts 由 lib 内部分配，订阅方直接读取。
 * 任意一次 RT 变更事件都携带该方向的全量列表（覆盖语义）。
 */
typedef struct vrf_api_af
{
    uint16_t afi;             /**< VRF_AFI_* */
    uint8_t safi;             /**< VRF_SAFI_* */
    uint8_t has_rd;           /**< 1=已配置 RD */
    vrf_rd_t rd;              /**< RD（has_rd=1 时有效） */
    uint16_t import_rt_count; /**< import RT 数量 */
    uint16_t export_rt_count; /**< export RT 数量 */
    vrf_rt_t *import_rts;     /**< import RT 数组（lib 持有） */
    vrf_rt_t *export_rts;     /**< export RT 数组（lib 持有） */
} vrf_api_af_t;

/**
 * @brief VRF 缓存条目（按 vrf_id 聚合）
 */
typedef struct vrf_api_cache_entry
{
    uint32_t vrf_id;             /**< VRF ID */
    char name[VRF_NAME_MAX_LEN]; /**< VRF 名称 */
    uint32_t l3vrf_table_id;     /**< OS L3VRF 路由表 ID */
    uint8_t os_state;            /**< VRF_OS_STATE_* */
    uint8_t _pad[3];             /**< 对齐填充 */
    GHashTable *afs;             /**< (afi<<8|safi) → vrf_api_af_t*（lib 持有） */
} vrf_api_cache_entry_t;

/**
 * @brief 缓存遍历回调
 * @return TRUE 表示停止遍历，FALSE 继续
 */
typedef gboolean (*vrf_api_cache_iter_fn)(const vrf_api_cache_entry_t *entry, void *user_data);

// ============================================================================
// IPC 协议（大类 = DEV_IPC_CATEGORY_VRF = 0x0007）
// ============================================================================

/** RPC 请求：根据 VRF ID 查询 VRF 名称；payload = uint32_t vrf_id */
#define VRF_MSG_TYPE_GET_NAME DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_VRF, 0x0001)

/** 订阅 VRF 事件；payload = vrf_subscribe_req_t */
#define VRF_MSG_TYPE_SUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_VRF, 0x0010)

/** 取消订阅；payload = vrf_subscribe_req_t（mask 全 0 表示全部取消） */
#define VRF_MSG_TYPE_UNSUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_VRF, 0x0011)

/** VRF 事件通知；payload = vrf_event_msg_t（含变长 RT 列表） */
#define VRF_MSG_TYPE_EVENT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_VRF, 0x0012)

/** 通用 ACK；payload = vrf_msg_ack_t */
#define VRF_MSG_TYPE_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_VRF, 0x001F)

/** RPC 响应：payload = vrf_name 字符串（null 结尾），未找到则 payload_len=0 */
#define VRF_MSG_TYPE_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_VRF, 0x00FF)

// ----------------------------------------------------------------------------
// 事件位图（vrf_event_msg_t::event 取值；订阅 mask 用同一组宏）
// ----------------------------------------------------------------------------

/** VRF 容器创建 */
#define VRF_EVENT_VRF_ADD (1u << 0)
/** VRF 容器删除 */
#define VRF_EVENT_VRF_DEL (1u << 1)
/** VRF OS 状态变更（os_state 字段有效） */
#define VRF_EVENT_VRF_STATE (1u << 2)
/** AF 启用（首次出现某 afi/safi） */
#define VRF_EVENT_AF_ENABLE (1u << 3)
/** AF 关闭（最后一个 afi/safi 配置被清空） */
#define VRF_EVENT_AF_DISABLE (1u << 4)
/** AF 下 RD 变更（has_rd / rd 字段有效） */
#define VRF_EVENT_AF_RD_CHANGE (1u << 5)
/** AF 下 import RT 集合变更（rts[0..rt_count) 为新全量列表） */
#define VRF_EVENT_AF_IMPORT_RT_CHG (1u << 6)
/** AF 下 export RT 集合变更（rts[0..rt_count) 为新全量列表） */
#define VRF_EVENT_AF_EXPORT_RT_CHG (1u << 7)
/** 通配：匹配所有事件 */
#define VRF_EVENT_ALL 0xFFFFFFFFu

// ----------------------------------------------------------------------------
// AF 位图（订阅 af_mask 用）
// ----------------------------------------------------------------------------

/** AF 位图：IPv4 unicast */
#define VRF_AF_MASK_IPV4 (1u << 0)
/** AF 位图：IPv6 unicast */
#define VRF_AF_MASK_IPV6 (1u << 1)
/** AF 位图：通配（匹配所有 AF） */
#define VRF_AF_MASK_ALL 0xFFFFFFFFu

// ----------------------------------------------------------------------------
// 订阅标志
// ----------------------------------------------------------------------------

/** 订阅时立即按 mask 回放当前全量状态 */
#define VRF_SUBSCRIBE_FLAG_REPLAY (1u << 0)

// ----------------------------------------------------------------------------
// IPC 载荷结构
// ----------------------------------------------------------------------------

/**
 * @brief 订阅 / 取消订阅请求载荷
 */
typedef struct vrf_subscribe_req
{
    uint32_t af_mask;    /**< AF 位图（VRF_AF_MASK_*） */
    uint32_t event_mask; /**< 事件位图（VRF_EVENT_*） */
    uint32_t flags;      /**< 订阅标志（VRF_SUBSCRIBE_FLAG_*） */
} vrf_subscribe_req_t;

/**
 * @brief VRF 事件消息载荷
 *
 * 总长度 = sizeof(vrf_event_msg_t) + rt_count * sizeof(vrf_rt_t)。
 * 不同 event 字段使用规则：
 *   - VRF_ADD / VRF_DEL：vrf_id, name, l3vrf_table_id 有效；afi/safi=0
 *   - VRF_STATE       ：vrf_id, os_state 有效
 *   - AF_ENABLE/DISABLE：vrf_id, afi, safi 有效
 *   - AF_RD_CHANGE    ：vrf_id, afi, safi, has_rd, rd 有效
 *   - AF_IMPORT_RT_CHG / AF_EXPORT_RT_CHG：vrf_id, afi, safi, rt_count, rts[] 有效（覆盖语义）
 */
typedef struct vrf_event_msg
{
    uint32_t event;              /**< 单值事件类型（VRF_EVENT_* 某一位） */
    uint32_t vrf_id;             /**< VRF ID */
    uint16_t afi;                /**< 地址族（0=N/A） */
    uint8_t safi;                /**< 子地址族（0=N/A） */
    uint8_t os_state;            /**< OS 状态（VRF_OS_STATE_*） */
    uint32_t l3vrf_table_id;     /**< OS L3VRF 路由表 ID */
    char name[VRF_NAME_MAX_LEN]; /**< VRF 名称 */
    uint8_t has_rd;              /**< RD 是否有效 */
    uint8_t _pad;                /**< 对齐填充 */
    vrf_rd_t rd;                 /**< RD（has_rd=1 时有效） */
    uint16_t rt_count;           /**< rts[] 元素数 */
    vrf_rt_t rts[1];             /**< 变长 RT 数组（覆盖语义） */
} vrf_event_msg_t;

/**
 * @brief 通用 ACK 载荷
 */
typedef struct vrf_msg_ack
{
    int32_t result; /**< ERRCODE_SUCCESS=0 表示成功 */
} vrf_msg_ack_t;

// ============================================================================
// RPC API（请求-响应一次性调用）
// ============================================================================

/**
 * @brief 通过 VRF RPC 根据 VRF ID 获取 VRF 名称
 * @param ctx       调用方 IPC 上下文（需已连接到 VRF 模块）
 * @param vrf_id    VRF ID
 * @param name_out  输出 VRF 名称缓冲区
 * @param name_size 缓冲区大小（建议 VRF_NAME_MAX_LEN）
 * @return 成功返回 0，失败或未找到返回 -1
 */
int vrf_get_name(dev_ipc_context_t *ctx, uint32_t vrf_id, char *name_out, size_t name_size);

// ============================================================================
// lib API：客户端缓存 + 订阅（libvrf_api.so）
// ============================================================================

/**
 * @brief 初始化 VRF 客户端缓存（订阅前调用一次）
 */
void vrf_api_cache_init(void);

/**
 * @brief 清理 VRF 客户端缓存
 */
void vrf_api_cache_cleanup(void);

/**
 * @brief 用 VRF 事件更新缓存
 *
 * 订阅方在收到 VRF_MSG_TYPE_EVENT 消息时调用一次本函数即可完成缓存维护。
 */
void vrf_api_cache_on_event(const dev_ipc_message_t *msg);

/**
 * @brief 按 vrf_id 查询缓存条目（借用，仅当前缓存版本下有效）
 */
const vrf_api_cache_entry_t *vrf_api_cache_lookup(uint32_t vrf_id);

/**
 * @brief 按名称查询缓存条目
 */
const vrf_api_cache_entry_t *vrf_api_cache_lookup_by_name(const char *name);

/**
 * @brief 查询指定 (vrf_id, afi, safi) 的 AF 配置
 * @return AF 配置指针（借用），未配置返回 NULL
 */
const vrf_api_af_t *vrf_api_cache_get_af(uint32_t vrf_id, uint16_t afi, uint8_t safi);

/**
 * @brief 遍历所有缓存条目
 */
void vrf_api_cache_foreach(vrf_api_cache_iter_fn iter_fn, void *user_data);

/**
 * @brief 向 VRF 模块发送订阅请求
 * @param ctx        调用方 IPC 上下文（需已连接到 VRF 模块）
 * @param af_mask    关心的 AF 位图（VRF_AF_MASK_*）
 * @param event_mask 关心的事件位图（VRF_EVENT_*）
 * @param flags      订阅标志（VRF_SUBSCRIBE_FLAG_*）
 * @return ERRCODE_SUCCESS / ERRCODE_FAIL
 */
int vrf_api_subscribe(dev_ipc_context_t *ctx, uint32_t af_mask, uint32_t event_mask, uint32_t flags);

/**
 * @brief 订阅全部 AF 与全部事件，并请求初始全量回放
 */
int vrf_api_subscribe_all(dev_ipc_context_t *ctx);

/**
 * @brief 取消订阅（mask 全 0 表示清除该模块的全部订阅）
 */
int vrf_api_unsubscribe(dev_ipc_context_t *ctx, uint32_t af_mask, uint32_t event_mask);

#endif /* VRF_H */
