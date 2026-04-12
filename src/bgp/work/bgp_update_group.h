/**
 * @file   bgp_update_group.h
 * @brief  BGP Update Group / NH Subgroup：按出向策略与 nexthop 策略分组的发布单元
 * @author jhb
 * @date   2026/04/10
 *
 * 分组目的：
 *   1. 出向策略相同的邻居共享属性准备结果（避免每邻居单独 prepare_attr）
 *   2. nexthop 策略相同的邻居进一步共享 MP_REACH 报文（nexthop 一致才能同报文）
 *   3. 每个 NH 子组持有独立 Adj-RIB-Out，支持增量发布
 *
 * 层级：
 *   bgp_instance
 *     └── update_groups (GList)
 *           └── subgroups (GList)
 *                 ├── session_list (GList，借用)
 *                 ├── adj_rib_out
 *                 └── announce_queue / withdraw_queue
 */
#ifndef BGP_UPDATE_GROUP_H
#define BGP_UPDATE_GROUP_H

#include <glib.h>
#include <stdint.h>

#include "bgp_adj_rib_out.h"
#include "bgp_session.h"
#include "net_addr.h"

/* 前向声明，避免循环包含 */
typedef struct bgp_instance bgp_instance_t;
typedef struct bgp_update_group bgp_update_group_t;
typedef struct bgp_nh_subgroup bgp_nh_subgroup_t;

/**
 * @brief NH 策略类型
 */
typedef enum bgp_nh_policy
{
    BGP_NH_POLICY_DEFAULT = 0,   /**< 默认：BGP 路由保持原 nh，import 路由用本端地址 */
    BGP_NH_POLICY_SELF = 1,      /**< 所有路由用本端地址（next-hop-self） */
    BGP_NH_POLICY_UNCHANGED = 2, /**< 所有路由保持原 nh（next-hop-unchanged） */
    BGP_NH_POLICY_SET = 3,       /**< 所有路由用指定地址（next-hop set） */
} bgp_nh_policy_t;

/**
 * @brief Update Group 键（出向策略级别）
 */
typedef struct bgp_update_group_key
{
    bgp_sess_type_t sess_type; /**< iBGP / eBGP（不同类型走不同属性准备逻辑） */
    uint32_t policy_hash;      /**< 出向 route-map 哈希（Phase 4 启用；当前恒为 0） */
} bgp_update_group_key_t;

/**
 * @brief NH Subgroup 键（nexthop 策略级别）
 */
typedef struct bgp_nh_subgroup_key
{
    bgp_nh_policy_t nh_policy;       /**< nexthop 策略 */
    net_addr_t effective_local_addr; /**< 本端连接地址（ESTABLISHED 时由 bgp_conn_get_local_addr 缓存） */
} bgp_nh_subgroup_key_t;

/**
 * @brief NH 子组：持有 Adj-RIB-Out 与发布队列
 */
struct bgp_nh_subgroup
{
    bgp_nh_subgroup_key_t key;      /**< 子组键 */
    bgp_update_group_t *parent;     /**< 所属 update group（借用引用） */
    GList *session_list;            /**< bgp_session_t*（借用引用，不持有所有权） */
    uint32_t session_count;         /**< session_list 长度缓存 */
    bgp_adj_rib_out_t *adj_rib_out; /**< 出向路由表（持有所有权；Phase 1 暂为 NULL） */
    GQueue *announce_queue;         /**< 待宣告 NLRI 队列（元素为 bgp_nlri_entry_t 堆副本） */
    GQueue *withdraw_queue;         /**< 待撤销 NLRI 队列（元素为 bgp_nlri_entry_t 堆副本） */
    uint32_t announce_count;        /**< 累计宣告计数（调试用） */
    uint32_t withdraw_count;        /**< 累计撤销计数（调试用） */
};

/**
 * @brief Update Group：按出向策略聚合的邻居集合
 */
struct bgp_update_group
{
    bgp_update_group_key_t key; /**< 组键 */
    bgp_instance_t *inst;       /**< 所属 AF 实例（借用引用） */
    GList *subgroups;           /**< bgp_nh_subgroup_t*（持有所有权） */
    uint32_t subgroup_count;    /**< subgroups 长度缓存 */
    uint32_t group_id;          /**< 自增 ID（调试用） */
};

/**
 * @brief 计算 session 的 update group 键
 * @param sess 目标 session（不可为 NULL）
 * @param out  输出键
 */
void bgp_session_compute_ug_key(const bgp_session_t *sess, bgp_update_group_key_t *out);

/**
 * @brief 计算 session 的 nh subgroup 键
 *
 * eBGP 默认 nh_policy=SELF，iBGP 默认 UNCHANGED。
 * effective_local_addr 通过 bgp_conn_get_local_addr(sess->pri_conn) 获取；
 * 若无有效连接，family 置 0。
 *
 * @param sess 目标 session（不可为 NULL）
 * @param out  输出键
 */
void bgp_session_compute_sg_key(const bgp_session_t *sess, bgp_nh_subgroup_key_t *out);

/**
 * @brief 在实例下查找或创建 update group
 * @param inst 所属实例
 * @param key  组键
 * @return update group 指针（持有在 inst->update_groups 中）
 */
bgp_update_group_t *bgp_update_group_find_or_create(bgp_instance_t *inst, const bgp_update_group_key_t *key);

/**
 * @brief 销毁 update group（连同所有 subgroup）
 * @param ug update group 指针（允许为 NULL）
 */
void bgp_update_group_destroy(bgp_update_group_t *ug);

/**
 * @brief 若 update group 已无 subgroup，将其从 inst 链表中摘除并销毁
 */
void bgp_update_group_remove_if_empty(bgp_instance_t *inst, bgp_update_group_t *ug);

/**
 * @brief 在 update group 下查找或创建 subgroup
 */
bgp_nh_subgroup_t *bgp_nh_subgroup_find_or_create(bgp_update_group_t *ug, const bgp_nh_subgroup_key_t *key);

/**
 * @brief 销毁 subgroup（释放队列、adj_rib_out、session_list）
 * @param sg subgroup 指针（允许为 NULL）
 */
void bgp_nh_subgroup_destroy(bgp_nh_subgroup_t *sg);

/**
 * @brief 若 subgroup 已无 session，将其从 ug->subgroups 中摘除并销毁
 */
void bgp_nh_subgroup_remove_if_empty(bgp_update_group_t *ug, bgp_nh_subgroup_t *sg);

/**
 * @brief session 加入 subgroup（幂等：若已在某 subgroup，先 leave 再 join）
 * @param inst AF 实例
 * @param sess 目标 session
 */
void bgp_subgroup_session_join(bgp_instance_t *inst, bgp_session_t *sess);

/**
 * @brief session 离开 subgroup（幂等：sess->subgroup==NULL 时直接返回）
 * @param inst AF 实例
 * @param sess 目标 session
 */
void bgp_subgroup_session_leave(bgp_instance_t *inst, bgp_session_t *sess);

/**
 * @brief subgroup 遍历回调
 */
typedef void (*bgp_subgroup_cb)(bgp_nh_subgroup_t *sg, gpointer user_data);

/**
 * @brief 遍历实例下所有 subgroup
 */
void bgp_instance_foreach_subgroup(bgp_instance_t *inst, bgp_subgroup_cb cb, gpointer user_data);

#endif /* BGP_UPDATE_GROUP_H */
