/**
 * @file rpm.h
 * @brief Routing Policy Manager 公共协议与客户端 API
 */
#ifndef RPM_H
#define RPM_H

#include <stdbool.h>
#include <stdint.h>

#include "dev.h"
#include "net_addr.h"

#define RPM_POLICY_NAME_MAX 64
#define RPM_POLICY_MAX_NODES 32
#define RPM_COMMUNITY_MAX 256

/*
 * RPM 对象类别采用 bitmask。业务模块用它声明关注哪些配置对象，
 * 策略本身不携带 BGP export/import 等业务用途。
 */
#define RPM_OBJECT_ROUTE_POLICY (1u << 0)
#define RPM_OBJECT_PREFIX_LIST (1u << 1)
#define RPM_OBJECT_COMMUNITY_FILTER (1u << 2)
#define RPM_OBJECT_AS_PATH_FILTER (1u << 3)
#define RPM_OBJECT_ALL 0xFFFFFFFFu

/* 节点匹配条件 bitmask。match_mask=0 表示无条件匹配。 */
#define RPM_MATCH_PREFIX (1u << 0)

/* 节点动作 bitmask。 */
#define RPM_APPLY_LOCAL_PREF (1u << 0)
#define RPM_APPLY_MED (1u << 1)
#define RPM_APPLY_COMMUNITY (1u << 2)

#define RPM_MSG_TYPE_SUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_RPM, 0x0001)
#define RPM_MSG_TYPE_UNSUBSCRIBE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_RPM, 0x0002)
#define RPM_MSG_TYPE_POLICY_GET DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_RPM, 0x0003)
#define RPM_MSG_TYPE_POLICY_EVENT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_RPM, 0x0004)
#define RPM_MSG_TYPE_ACK DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_RPM, 0x0005)

typedef enum rpm_policy_event_type
{
    RPM_POLICY_EVENT_UPSERT = 1,
    RPM_POLICY_EVENT_DELETE = 2,
    RPM_POLICY_EVENT_SMOOTH_END = 3,
} rpm_policy_event_type_t;

typedef enum rpm_policy_decision
{
    RPM_POLICY_DECISION_DENY = 0,
    RPM_POLICY_DECISION_PERMIT = 1,
} rpm_policy_decision_t;

typedef struct rpm_policy_node
{
    uint32_t sequence;
    uint8_t permit;
    uint8_t reserved[3];
    uint32_t match_mask;
    net_prefix_t prefix;
    uint32_t apply_mask;
    uint32_t local_pref;
    uint32_t med;
    char community[RPM_COMMUNITY_MAX];
} rpm_policy_node_t;

typedef struct rpm_policy
{
    char name[RPM_POLICY_NAME_MAX];
    uint32_t revision;
    uint32_t node_count;
    rpm_policy_node_t nodes[RPM_POLICY_MAX_NODES];
} rpm_policy_t;

typedef struct rpm_subscribe_req
{
    uint32_t interest_mask;
    uint32_t flags;
} rpm_subscribe_req_t;

#define RPM_SUBSCRIBE_FLAG_REPLAY (1u << 0)

typedef struct rpm_policy_get_req
{
    char name[RPM_POLICY_NAME_MAX];
} rpm_policy_get_req_t;

typedef struct rpm_policy_get_resp
{
    uint8_t found;
    uint8_t reserved[3];
    rpm_policy_t policy;
} rpm_policy_get_resp_t;

typedef struct rpm_policy_event
{
    uint32_t event;
    uint32_t object_mask;
    rpm_policy_t policy;
} rpm_policy_event_t;

typedef struct rpm_policy_result
{
    uint32_t apply_mask;
    uint32_t local_pref;
    uint32_t med;
    char community[RPM_COMMUNITY_MAX];
} rpm_policy_result_t;

/**
 * 订阅 RPM 对象事件。interest_mask 是对象类别位图，RPM 仅推送有交集的对象。
 */
int rpm_api_subscribe(dev_ipc_context_t *ctx, uint32_t interest_mask, uint32_t flags);
int rpm_api_unsubscribe(dev_ipc_context_t *ctx);

/**
 * 查询指定名称的通用 route-policy。不存在时返回 ERRCODE_SUCCESS，
 * 并将 out->found 置 0。
 */
int rpm_api_policy_get(dev_ipc_context_t *ctx, const char *name, rpm_policy_get_resp_t *out);

/**
 * 本地求值 helper。节点按 sequence 升序首个命中即终止；全部未命中隐式 deny。
 */
rpm_policy_decision_t rpm_policy_evaluate(const rpm_policy_t *policy, const net_prefix_t *prefix,
                                          rpm_policy_result_t *result);

#endif /* RPM_H */
