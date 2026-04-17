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
 *                 ├── peer_list (GList，借用)
 *                 ├── adj_rib_out
 *                 └── announce_queue / withdraw_queue
 */
#ifndef BGP_UPDATE_GROUP_H
#define BGP_UPDATE_GROUP_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "bgp_adj_rib_out.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "net_addr.h"

/* 前向声明，避免循环包含 */
typedef struct bgp_instance bgp_instance_t;
typedef struct bgp_update_group bgp_update_group_t;
typedef struct bgp_nh_subgroup bgp_nh_subgroup_t;
typedef struct bgp_peer bgp_peer_t;
typedef struct bgp_route_node bgp_route_node_t;
typedef struct bgp_attr bgp_attr_t;
typedef struct bgp_nexthop bgp_nexthop_t;

/**
 * @brief NH 计算规则（子组维度）
 *
 * 每条具体路由根据其来源类型（IMPORT / eBGP-learned / iBGP-learned）+ 目标 ug 的
 * sess_type，被分派到某一个规则对应的子组。同一个 peer 可同时属于多个子组。
 */
typedef enum bgp_nh_rule
{
    BGP_NH_RULE_LOCAL = 0,  /**< 使用本端连接地址（next-hop-self 等价） */
    BGP_NH_RULE_PASS = 1,   /**< 保留原 nh（next-hop-unchanged 等价） */
    BGP_NH_RULE_CONFIG = 2, /**< 使用用户配置地址（预留） */
} bgp_nh_rule_t;

/**
 * @brief 路由来源分类
 */
typedef enum bgp_route_src_class
{
    BGP_RSRC_IMPORT = 0,    /**< 本地引入（route-import / 静态注入） */
    BGP_RSRC_FROM_EBGP = 1, /**< 从 eBGP 邻居学习 */
    BGP_RSRC_FROM_IBGP = 2, /**< 从 iBGP 邻居学习 */
} bgp_route_src_class_t;

/**
 * @brief Update Group 键（出向策略级别）
 */
typedef struct bgp_update_group_key
{
    bgp_sess_type_t sess_type; /**< iBGP / eBGP（不同类型走不同属性准备逻辑） */
    uint32_t policy_hash;      /**< 出向 route-map 哈希（Phase 4 启用；当前恒为 0） */
    uint16_t peer_family;      /**< peer 地址族（AF_INET / AF_INET6，用于区分双栈邻居） */
} bgp_update_group_key_t;

/**
 * @brief NH Subgroup 键（nexthop 计算规则级别）
 *
 * effective_local_addr 仅对 BGP_NH_RULE_LOCAL 有意义；其他 rule 下 family=0。
 */
typedef struct bgp_nh_subgroup_key
{
    bgp_nh_rule_t rule;              /**< nexthop 计算规则 */
    net_addr_t effective_local_addr; /**< 本端连接地址（仅 R_LOCAL 使用；否则 family=0） */
} bgp_nh_subgroup_key_t;

/**
 * @brief NH 子组：持有 Adj-RIB-Out 与发布队列
 */
struct bgp_nh_subgroup
{
    bgp_nh_subgroup_key_t key;      /**< 子组键 */
    bgp_update_group_t *parent;     /**< 所属 update group（借用引用） */
    GList *peer_list;               /**< bgp_peer_t*（借用引用，不持有所有权） */
    uint32_t peer_count;            /**< peer_list 长度缓存 */
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
 * @brief 销毁 subgroup（释放队列、adj_rib_out、peer_list）
 * @param sg subgroup 指针（允许为 NULL）
 */
void bgp_nh_subgroup_destroy(bgp_nh_subgroup_t *sg);

/**
 * @brief 若 subgroup 已无 peer，将其从 ug->subgroups 中摘除并销毁
 */
void bgp_nh_subgroup_remove_if_empty(bgp_update_group_t *ug, bgp_nh_subgroup_t *sg);

/**
 * @brief peer 加入其 update_group 下所有适用的子组（幂等）
 *
 * 规则：peer 所在的 update_group 根据 sess_type 决定需要哪些 rule 的子组：
 *   eBGP: { LOCAL }
 *   iBGP: { LOCAL, PASS }
 *
 * @param peer 目标 peer（多子组归属，borrow 指针列表挂在 peer->subgroups）
 * @param sess 目标 session（用于键计算与日志）
 */
void bgp_subgroup_peer_join(bgp_peer_t *peer, bgp_session_t *sess);

/**
 * @brief peer 离开其所有子组（幂等：peer->subgroups==NULL 时直接返回）
 * @param peer 目标 peer
 * @param sess 目标 session（用于日志，可为 NULL）
 */
void bgp_subgroup_peer_leave(bgp_peer_t *peer, bgp_session_t *sess);

/**
 * @brief 路由来源分类（是否 IMPORT / 学自 eBGP / 学自 iBGP）
 * @param best 最优路由节点
 */
bgp_route_src_class_t bgp_classify_route_src(const bgp_route_node_t *best);

/**
 * @brief 为 (ug, 路由来源类别) 选择 nh 计算规则
 *
 * eBGP 目标：任何来源 → LOCAL
 * iBGP 目标：IMPORT → LOCAL；FROM_EBGP → PASS；FROM_IBGP → 过滤（split-horizon，返回 -1）
 *
 * @param ug_key  目标 ug 键
 * @param src_class 路由来源分类
 * @param out_rule 输出的 rule
 * @return true=可发布；false=过滤（split-horizon 或不适用）
 */
bool bgp_select_nh_rule(const bgp_update_group_key_t *ug_key, bgp_route_src_class_t src_class, bgp_nh_rule_t *out_rule);

/**
 * @brief 在 ug 中查找某 rule 对应的子组（peer 加入后必然存在）
 * @return subgroup 指针；未找到返回 NULL
 */
bgp_nh_subgroup_t *bgp_update_group_find_subgroup_by_rule(const bgp_update_group_t *ug, bgp_nh_rule_t rule);

/**
 * @brief subgroup 遍历回调
 */
typedef void (*bgp_subgroup_cb)(bgp_nh_subgroup_t *sg, gpointer user_data);

/**
 * @brief 遍历实例下所有 subgroup
 */
void bgp_instance_foreach_subgroup(bgp_instance_t *inst, bgp_subgroup_cb cb, gpointer user_data);

// ============================================================================
// Subgroup 级发布通路（事件 + 发送）
// ============================================================================

/**
 * @brief 子组级出向策略评估
 *
 * 整合属性准备与 nexthop 策略：
 *   1. 复制 best 属性到 out_attr
 *   2. eBGP 场景下在 AS_PATH 首部 prepend 本地 AS
 *   3. 按子组 nh_policy / effective_local_addr 生成 out_nh
 *   4. iBGP→iBGP 反射检查（整个子组 sess_type 一致）
 *
 * Per-session 检查（split-horizon、AS_PATH 防环）延后到发送阶段执行。
 *
 * @return true=应宣告, false=组级策略拒绝
 */
bool bgp_subgroup_eval_export(const bgp_nh_subgroup_t *sg, const bgp_route_node_t *best, const bgp_nlri_entry_t *nlri,
                              bgp_attr_t *out_attr, bgp_nexthop_t *out_nh);

/**
 * @brief 将一条 best-route 变更挂入该实例所有 subgroup 的 announce_queue
 */
void bgp_update_group_enqueue_announce(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 将一条 NLRI 撤销挂入该实例所有 subgroup 的 withdraw_queue
 */
void bgp_update_group_enqueue_withdraw(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri);

/**
 * @brief 邻居进入 ESTABLISHED 后，将其加入各 AF subgroup 并补发路由
 */
void bgp_update_group_catchup_session(bgp_session_t *sess);

/**
 * @brief 批量处理 subgroup 的 announce/withdraw 队列（打包发送）
 *
 * 先处理 withdraw_queue，再处理 announce_queue；announce 按 (attr_ref, nh)
 * 分桶打包发送，per-session 过滤 split-horizon 与 AS_PATH 防环。
 *
 * @return 实际处理的 NLRI 总数
 */
int bgp_subgroup_process_queues(bgp_nh_subgroup_t *sg, bgp_instance_t *inst, int batch_size);

/**
 * @brief 处理一条 BGP session-pub 工作事件（worker 线程调用）
 */
void bgp_update_group_handle_pub_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 在当前线程同步抽干 inst 下所有 subgroup 队列（不允许重新调度事件）
 * @return 实际处理条目数
 */
int bgp_update_group_process_pending(bgp_instance_t *inst);

#endif /* BGP_UPDATE_GROUP_H */
