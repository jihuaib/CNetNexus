/**
 * @file srv6.h
 * @brief SRv6 locator 与本地 service SID 公共 IPC/API。
 */
#ifndef SRV6_H
#define SRV6_H

#include <stdint.h>

#include "dev.h"
#include "net_addr.h"

#define SRV6_LOCATOR_NAME_MAX 64u
#define SRV6_RPC_DEFAULT_TIMEOUT_MS 5000u

/* RFC 8986 / IANA Endpoint Behavior codepoints。数值与 fib_srv6_behavior_t 对齐。 */
typedef enum srv6_behavior
{
    SRV6_BEHAVIOR_END_DT6 = 18,
    SRV6_BEHAVIOR_END_DT4 = 19,
} srv6_behavior_t;

#define SRV6_MSG_TYPE_SID_ALLOC DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x0001)
#define SRV6_MSG_TYPE_SID_RELEASE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x0002)
#define SRV6_MSG_TYPE_SID_GET DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x0003)
#define SRV6_MSG_TYPE_SID_RESULT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x0004)
#define SRV6_MSG_TYPE_LOCATOR_GET DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x0005)
#define SRV6_MSG_TYPE_LOCATOR_RESULT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x0006)
#define SRV6_MSG_TYPE_SID_RELEASE_OWNER DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SRV6, 0x0007)

typedef struct srv6_locator_query
{
    char name[SRV6_LOCATOR_NAME_MAX];
} srv6_locator_query_t;

typedef struct srv6_locator_result
{
    int32_t result;
    uint8_t found;
    uint8_t _pad0[3];
} srv6_locator_result_t;

/**
 * SID 分配键。owner_module_id 由客户端库和服务端共同规范为请求源模块，
 * 防止跨模块释放；owner_id 由业务模块定义且必须在该模块内稳定。
 */
typedef struct srv6_sid_key
{
    char locator[SRV6_LOCATOR_NAME_MAX];
    uint32_t vrf_id;
    uint16_t behavior; /**< srv6_behavior_t */
    uint16_t _pad0;
    uint32_t owner_module_id;
    uint32_t owner_id;
} srv6_sid_key_t;

/** 已分配的 SRv6 service SID。 */
typedef struct srv6_sid_entry
{
    srv6_sid_key_t key;
    uint32_t function_id;
    uint8_t prefix_len; /**< service SID 当前固定为 /128 */
    uint8_t _pad0[3];
    net_addr_t sid;
} srv6_sid_entry_t;

/** SID RPC 响应。result 为 ERRCODE_*；found=1 时 entry 有效。 */
typedef struct srv6_sid_result
{
    int32_t result;
    uint8_t found;
    uint8_t _pad0[3];
    srv6_sid_entry_t entry;
} srv6_sid_result_t;

/**
 * SID owner-scope cleanup key.  The server derives owner_module_id from the
 * authenticated IPC source; callers can only reconcile their own bindings.
 */
typedef struct srv6_sid_owner_scope
{
    uint32_t vrf_id;
    uint16_t behavior; /**< srv6_behavior_t */
    uint16_t _pad0;
    uint32_t owner_id;
} srv6_sid_owner_scope_t;

/**
 * 幂等分配并同步安装 End.DT localsid。只有持久化与 FIB 安装均成功才返回成功；
 * 同一个 key 在 release 前始终返回相同 SID。
 */
int srv6_rpc_sid_alloc(dev_ipc_context_t *ctx, const srv6_sid_key_t *key, srv6_sid_entry_t *entry_out,
                       uint32_t timeout_ms);

/** 同步删除 FIB localsid 后释放持久绑定；不存在视为成功。 */
int srv6_rpc_sid_release(dev_ipc_context_t *ctx, const srv6_sid_key_t *key, uint32_t timeout_ms);

/**
 * 同步释放调用模块在指定 VRF/behavior/owner-id 下的全部 binding。
 * 用于配置回滚和冷启动清理 locator key 已不可恢复的孤儿 SID；不存在视为成功。
 */
int srv6_rpc_sid_release_owner(dev_ipc_context_t *ctx, const srv6_sid_owner_scope_t *scope, uint32_t timeout_ms);

/** 查询已有绑定；返回成功前会确认 localsid 已安装到 FIB。 */
int srv6_rpc_sid_get(dev_ipc_context_t *ctx, const srv6_sid_key_t *key, srv6_sid_entry_t *entry_out,
                     uint32_t timeout_ms);

/** 查询 locator 是否已配置；不存在返回 ERRCODE_DEP_MISSING。 */
int srv6_rpc_locator_exists(dev_ipc_context_t *ctx, const char *name, uint32_t timeout_ms);

const char *srv6_behavior_name(uint16_t behavior);

#endif /* SRV6_H */
