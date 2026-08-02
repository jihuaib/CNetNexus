/**
 * @file   bgp_rib.h
 * @brief  BGP 内存 RIB 通用结构：rthead（前缀头）+ route（路径）
 * @author jhb
 * @date   2026/03/13
 */
#ifndef BGP_RIB_H
#define BGP_RIB_H

#include <glib.h>
#include <stdint.h>

#include "bgp.h"
#include "bgp_attr_intern.h"
#include "bit.h"
#include "net_addr.h"

/* 前向声明：bgp_instance_t / bgp_rd_entry_t 与 bgp_rib_t 相互引用 */
typedef struct bgp_instance bgp_instance_t;
typedef struct bgp_rd_entry bgp_rd_entry_t;
typedef struct bgp_rthead bgp_rthead_t;

/** 路由标记位：当前最优路径（由 bgp_calc 置位，需同时满足位于链表首位） */
#define BGP_ROUTE_FLAG_BEST (1U << 0)
/** 路由标记位：本地导入路由（非 BGP 邻居学习，由 import-route 引入） */
#define BGP_ROUTE_FLAG_IMPORT (1U << 1)
/** 路由标记位：nexthop 迭代有效（valid） */
#define BGP_ROUTE_FLAG_VALID (1U << 2)
/** 路由标记位：已下刷到 ROUTE 模块 */
#define BGP_ROUTE_FLAG_FLUSHED (1U << 3)
/** 路由标记位：已逻辑删除，待下刷撤销后物理清理 */
#define BGP_ROUTE_FLAG_STALE (1U << 4)
/** 路由标记位：禁止对外通告（来自 import-route 的 ETH 直连等场景，仅用于本地优选/迭代） */
#define BGP_ROUTE_FLAG_NO_ADV (1U << 5)
/** 路由标记位：import-rib 镜像路由（mirror 节点本身置位；源节点不置位） */
#define BGP_ROUTE_FLAG_IMPORT_RIB (1U << 6)
/** 路由标记位：本地跨表合成路由，两种来源共用此标记，按所在 instance 区分：
 *  (1) vrf-export：本地 VRF 单播 → 同 AF VPN（合成节点位于 public VPN inst）；
 *  (2) vrf 本地交叉：本机另一 VRF 单播 → 本 VRF 单播（合成节点位于私网 unicast inst，
 *      源 VRF export-RT 直接命中本 VRF import-RT 泄漏而来）。
 *  二者都属本地起源、正常对外通告；与 IMPORT(重分发) 区分，避免被 import-route 清理/再导入误伤。
 *  单跳防环：LOCAL_CROSS（已泄漏）不再作为本地交叉的泄漏源，也不被 vrf-export 回灌 vpnv4。 */
#define BGP_ROUTE_FLAG_LOCAL_CROSS (1U << 7)
/** 路由标记位：vrf-import 远端跨表合成路由（peer 的 VPN → 本地 VRF 单播）。非本地起源，
 *  绝不可被 vrf-export 回灌 VPN（否则成环） */
#define BGP_ROUTE_FLAG_REMOTE_CROSS (1U << 8)
/** 路由标记位：本地接收前缀（如接口/loopback 地址的 host route）。跨 VRF 本地泄漏到 ROUTE/FIB
 *  时需保留 local-delivery 语义，使目标 VRF 表安装 RTN_LOCAL 而不是 via 127.0.0.1 的 unicast。 */
#define BGP_ROUTE_FLAG_LOCAL_DELIVERY (1U << 9)
/** 路由标记位：desired 路径已变化，已下刷的旧 incarnation 仍由 FLUSHED 表示；
 *  新 best 可用后须以 ROUTE upsert 原地替换，成功后清除此位。 */
#define BGP_ROUTE_FLAG_FIB_DIRTY (1U << 10)
/** 路由标记位：REMOTE_CROSS 当前 incarnation 采用 SRv6 BE service-SID 转发。
 * 该位记录合成路由已经实际挂载的 watch 类型，不能由 instance 配置或 attr
 * 临时推导；模式切换时需先按旧值注销 watch，再更新此位并注册新 watch。 */
#define BGP_ROUTE_FLAG_SRV6_BE (1U << 11)
#define BGP_ROUTE_LABEL_SOURCE_NONE 0u
#define BGP_ROUTE_LABEL_SOURCE_LOCAL 1u
#define BGP_ROUTE_LABEL_SOURCE_RECEIVED 2u

/**
 * @brief 单条路径（同一 rthead 下可挂多条，按 source 来源地址区分）
 *
 * peer 路由：BGP_ROUTE_FLAG_IMPORT 未置位，source 为邻居 IP。
 * import 路由：BGP_ROUTE_FLAG_IMPORT 置位，source 为来源标识地址。
 * 有效路径：BGP_ROUTE_FLAG_VALID 置位。
 * 最优路径：BGP_ROUTE_FLAG_BEST + BGP_ROUTE_FLAG_VALID 均置位，且为链表首元素。
 */
typedef struct bgp_route_node
{
    bgp_rthead_t *head;        /**< 所属 rthead（借用引用） */
    net_addr_t source;         /**< 路由来源标识（peer 路由=邻居IP，import 路由=来源地址） */
    bgp_attr_ref_t *attr;      /**< 当前生效路径属性（共享引用，intern 后不可变） */
    bgp_attr_ref_t *base_attr; /**< peer 原始属性（未合入本 VRF export RT）；本地 import 路径为 NULL */
    gint64 added_at_usec;      /**< 路由首次加入时间（g_get_real_time，仅新增时写入） */
    gint64 updated_at_usec;    /**< 路由最近更新时间（g_get_real_time，每次 reach 写入） */
    uint32_t nexthop_id;       /**< ROUTE nexthop 对象 ID（key.nexthop 保存 BGP 下一跳地址） */
    uint32_t label;            /**< labeled-unicast 路径标签，语义由 label_source 区分 */
    uint8_t has_label;         /**< label 是否有效 */
    uint8_t label_source;      /**< BGP_ROUTE_LABEL_SOURCE_* */
    uint8_t _pad0[2];          /**< 对齐填充 */
    uint32_t flags;            /**< 路由标记位，见 BGP_ROUTE_FLAG_* */
    uint32_t import_proto;     /**< IMPORT 路由来源协议（非 import-route 为 0） */
    struct bgp_route_node *src_route; /**< mirror 节点指向源 labeled/VPN 节点；源节点为 NULL */
    uint32_t borrow_refcnt;           /**< 外部借用引用计数（import_rib mirror、bgp_relay watch 等） */
    /* inter-AS Option B 中转换标：本节点作为 vpnv4 中转路由(改下一跳为本端)时，向 TUNNEL 申请的
     * 本地入标签（advertise 给上游），TUNNEL 据此装 SWAP ILM（本地入标签→换成 label 出口转发）。 */
    uint32_t out_local_label;    /**< 本地分配并向上游通告的中转入标签（0=未分配） */
    uint32_t transit_owner_id;   /**< 向 TUNNEL 申请标签时的 owner_id（用于释放，0=未分配） */
    uint32_t transit_swap_label; /**< 当前 SWAP 绑定的对端出标签（变更时需重建绑定） */
    uint16_t transit_afi;        /**< SWAP 绑定的传输 AF，释放时必须与申请一致 */
    uint16_t _pad1;
    net_addr_t transit_endpoint; /**< SWAP 绑定的对端 BGP next-hop */
} bgp_route_node_t;

/**
 * @brief 是否本地起源路由：重分发(IMPORT) 或 vrf-export 本地跨表(LOCAL_CROSS)
 *
 * 对外通告/源分类/iBGP split-horizon/vpnv4 标签注入等"本地起源"判定一律用本谓词，
 * 这样 vrf-export 合成的 vpnv4 路由（LOCAL_CROSS）与重分发路由享同样的通告语义。
 * REMOTE_CROSS（vrf-import 自 peer）不属本地起源。
 */
static inline gboolean bgp_route_is_local_origin(const bgp_route_node_t *r)
{
    return r && (BIT_TEST(r->flags, BGP_ROUTE_FLAG_IMPORT) || BIT_TEST(r->flags, BGP_ROUTE_FLAG_LOCAL_CROSS));
}

/**
 * @brief 是否合成路由（非 peer 会话直接学习）：IMPORT / LOCAL_CROSS / REMOTE_CROSS 任一
 *
 * 合成路由的 source 是合成标识而非邻居 IP，不参与 peer 会话级清理（purge_source/flush_peer），
 * 也不作为 import-rib 的镜像来源。
 */
static inline gboolean bgp_route_is_synthetic(const bgp_route_node_t *r)
{
    return r && (BIT_TEST(r->flags, BGP_ROUTE_FLAG_IMPORT) || BIT_TEST(r->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) ||
                 BIT_TEST(r->flags, BGP_ROUTE_FLAG_REMOTE_CROSS));
}

/**
 * @brief 路由头（Route Head）：表示一个唯一 NLRI 前缀
 *
 * 树键为 NLRI 二进制内容（RIB 已按 AFI/SAFI 分实例）
 */
struct bgp_rthead
{
    bgp_nlri_entry_t nlri; /**< NLRI（前缀/EVPN/FlowSpec 等，含 afi/safi/type） */
    bgp_instance_t *inst;  /**< 所属 AF 实例（借用引用，可为 NULL） */
    struct bgp_rib *rib;   /**< 所属 RIB（借用引用，供物理回收时回查树/计数） */
    GList *route_list;     /**< bgp_route_node_t* 双向链表，首元素为当前最优路径 */
    uint32_t queue_refcnt; /**< 工作队列引用计数（>0 时禁止删除该 rthead） */
};

/**
 * @brief BGP 内存 RIB
 *
 * 每个 RIB 隶属于一个 RD entry；RD entry 又隶属于 (vrf, afi, safi) 实例。
 * 通过 rd_entry->inst 可回查 instance/vrf；rd_entry->key.rd 可拿到 RD。
 */
typedef struct bgp_rib
{
    GTree *head_tree;     /**< key = &head->nlri（直接指入值，无需堆分配），按 NLRI 二进制比较 */
    uint32_t head_count;  /**< rthead 总数 */
    uint32_t route_count; /**< route 总数（所有 rthead 下累计） */
    bgp_rd_entry_t *rd_entry; /**< 所属 RD entry（借用引用） */
} bgp_rib_t;

/**
 * @brief UPDATE 应用统计
 */
typedef struct bgp_rib_update_stats
{
    uint32_t reach_new;       /**< reach 新增路径数 */
    uint32_t reach_update;    /**< reach 覆盖更新路径数 */
    uint32_t unreach_removed; /**< unreach 成功删除路径数 */
    uint32_t unreach_miss;    /**< unreach 未命中路径数 */
} bgp_rib_update_stats_t;

/**
 * @brief 创建 RIB
 */
bgp_rib_t *bgp_rib_create(void);

/**
 * @brief 销毁 RIB
 */
void bgp_rib_destroy(bgp_rib_t *rib);

/**
 * @brief 对单条 NLRI 执行 unreach（删除一条来源路径）
 * @param rib      目标 RIB
 * @param nlri     NLRI 条目
 * @param source   路径来源（邻居 IP，二进制）
 * @return 1=删除成功, 0=未命中, -1=失败
 */
int bgp_rib_unreach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source);

/**
 * @brief 设置一条路径的 valid 状态
 * @param rib    目标 RIB
 * @param nlri   NLRI 条目
 * @param source 路径来源
 * @param valid  TRUE=有效，FALSE=无效
 * @return 1=状态有变化, 0=状态未变化/未命中, -1=失败
 */
int bgp_rib_set_route_valid(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source, gboolean valid);

/**
 * @brief 删除某来源在整个 RIB 下的 peer 路由（会清理空 rthead）
 * @param rib            目标 RIB
 * @param source         路径来源（邻居 IP，二进制）
 * @param removed_routes 输出：删除路径数（可为 NULL）
 * @param removed_heads  输出：删除 rthead 数（可为 NULL）
 */
void bgp_rib_remove_source(bgp_rib_t *rib, const net_addr_t *source, uint32_t *removed_routes, uint32_t *removed_heads);

/**
 * @brief 通过 NLRI 查找 rthead（只读）
 */
const bgp_rthead_t *bgp_rib_lookup_head(const bgp_rib_t *rib, const bgp_nlri_entry_t *nlri);

/**
 * @brief 通过 NLRI 查找或创建 rthead（可写）
 *
 * 当 NLRI 尚不存在时会创建空 rthead 并插入 RIB。
 */
bgp_rthead_t *bgp_rib_ensure_head(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri);

/**
 * @brief 在 rthead 下按 source 查找 route（可写）
 */
bgp_route_node_t *bgp_rthead_lookup_route_mut(bgp_rthead_t *head, const net_addr_t *source);

/**
 * @brief 在 rthead 下创建一条新 route（调用前应先确认未存在）
 *
 * 成功时自动将节点挂到 head->route_list 尾部，并递增 rib->route_count。
 */
bgp_route_node_t *bgp_rthead_create_route(bgp_rib_t *rib, bgp_rthead_t *head, const net_addr_t *source);

/**
 * @brief 对 route 应用 reach 更新（属性、nexthop、标志与时间戳）
 *
 * 不负责创建 rthead/route，仅更新已有 route。
 */
int bgp_rib_route_apply_reach(bgp_route_node_t *route, uint32_t import_proto, const bgp_attr_t *attr);
/**
 * @brief 设置 peer 原始属性快照
 *
 * 仅 peer 学习路径使用，用于后续按当前 VRF export RT 重建 effective attr。
 * 传入 NULL 会释放并清空已有 base_attr。
 */
int bgp_rib_route_set_base_attr(bgp_route_node_t *route, const bgp_attr_t *base_attr);
void bgp_route_set_label_from_nlri(bgp_route_node_t *route, const bgp_nlri_entry_t *nlri, uint8_t label_source);

/**
 * @brief 增加路径节点的外部借用引用计数
 *
 * 任何模块（import_rib、bgp_relay 等）若长期持有 bgp_route_node_t* 借用指针，
 * 必须调用本函数登记，避免节点在引用期间被 RIB 路径上的清理流程（unreach、purge）释放。
 */
void bgp_route_node_borrow_ref(bgp_route_node_t *route);

/**
 * @brief 释放路径节点的外部借用引用
 *
 * 借用期间节点即使被逻辑删除（标 STALE）也只会留在 RIB 链表上，不会被物理回收。
 * 当 refcnt 减为 0 且节点处于 STALE 状态时，本函数触发一次 reap：摘链 + 释放节点，
 * 并在 head 变空时一并销毁 head。调用者无需再次调用 free。
 */
void bgp_route_node_borrow_unref(bgp_route_node_t *route);

/**
 * @brief rthead 队列引用计数操作
 *
 * 入队时调用 bgp_rib_head_ref，出队处理完后调用 bgp_rib_head_unref。
 * 仅维护引用计数，不负责删除。
 */
void bgp_rib_head_ref(bgp_rthead_t *head);
void bgp_rib_head_unref(bgp_rthead_t *head);

/**
 * @brief 触发一个 rthead 的垃圾回收（删除 stale route，必要时删除空 rthead）
 *
 * 仅当 queue_refcnt 为 0 时执行物理删除。
 *
 * @param rib  目标 RIB
 * @param head 待回收的 rthead
 * @return 删除的 stale route 数量
 */
uint32_t bgp_rib_gc_head(bgp_rib_t *rib, bgp_rthead_t *head);

/**
 * @brief 遍历 RIB 中含有指定来源 peer 路径的所有 rthead，对每个 NLRI 触发回调
 *
 * 用于会话清理前收集受影响 NLRI，推送到 calc_queue 触发重新优选。
 * 本地 import-route（BGP_ROUTE_FLAG_IMPORT）会被忽略。
 *
 * @param rib       目标 RIB
 * @param source    路径来源（邻居 IP）
 * @param cb        回调函数，参数为 NLRI 指针（借用）和 user_data
 * @param user_data 传递给回调的上下文指针
 */
typedef void (*bgp_rib_source_nlri_cb)(const bgp_nlri_entry_t *nlri, gpointer user_data);
void bgp_rib_foreach_source(const bgp_rib_t *rib, const net_addr_t *source, bgp_rib_source_nlri_cb cb,
                            gpointer user_data);

/**
 * @brief 在 rthead 下按 source 查找 route（只读）
 */
const bgp_route_node_t *bgp_rthead_lookup_route(const bgp_rthead_t *head, const net_addr_t *source);

/**
 * @brief 获取统计值
 */
uint32_t bgp_rib_head_count(const bgp_rib_t *rib);
uint32_t bgp_rib_route_count(const bgp_rib_t *rib);

/**
 * @brief 将指定路径节点标记为最优（置 BGP_ROUTE_FLAG_BEST 并移至链表首位）
 *
 * 同一 rthead 下其余路径的 BGP_ROUTE_FLAG_BEST 均被清除。
 *
 * @param rib        目标 RIB
 * @param nlri       NLRI 匹配键（用于定位 rthead）
 * @param best_route 待置为最优的路径节点（须属于该 rthead 的 route_list）
 */
void bgp_rib_mark_best(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, bgp_route_node_t *best_route);

/**
 * @brief 查找当前最优路径（只读）
 *
 * 最优路径须同时满足：位于 route_list 首位，且具有 BGP_ROUTE_FLAG_BEST + BGP_ROUTE_FLAG_VALID 标记。
 *
 * @param rib  目标 RIB
 * @param nlri NLRI 匹配键
 * @return 最优路径指针（借用，不可释放），未找到返回 NULL
 */
const bgp_route_node_t *bgp_rib_find_best(const bgp_rib_t *rib, const bgp_nlri_entry_t *nlri);

/**
 * @brief 遍历回调类型：对每条带 BGP_ROUTE_FLAG_BEST 的路径调用
 *
 * @param head      路径所属的前缀头（借用）
 * @param route     带 BGP_ROUTE_FLAG_BEST 的路径（借用）
 * @param user_data 调用方上下文指针
 */
typedef void (*bgp_rib_best_cb)(const bgp_rthead_t *head, const bgp_route_node_t *route, gpointer user_data);

/**
 * @brief 遍历 RIB 中所有带 BGP_ROUTE_FLAG_BEST 的路径，对每条调用回调
 *
 * @param rib       目标 RIB
 * @param cb        回调函数
 * @param user_data 传递给回调的上下文指针
 */
void bgp_rib_foreach_best(const bgp_rib_t *rib, bgp_rib_best_cb cb, gpointer user_data);

/**
 * @brief 分片遍历回调：对一个 rthead 调用
 *
 * 回调返回 TRUE 表示继续本批处理，FALSE 表示提前停止。
 */
typedef gboolean (*bgp_rib_head_walk_cb)(bgp_rthead_t *head, gpointer user_data);

/**
 * @brief 从 last_nlri 之后分片遍历 RIB 的 rthead
 *
 * 断点按业务 key 保存，而不是保存 GTree 内部迭代状态。若 last_nlri 对应 head 已删除，
 * 遍历会继续处理第一个 key 大于 last_nlri 的 head。
 *
 * @param rib          目标 RIB
 * @param last_nlri    上次已处理的 NLRI；has_last 为 FALSE 时忽略
 * @param has_last     是否存在断点
 * @param budget       本批最多处理的 head 数；0 表示不处理
 * @param cb           head 处理回调
 * @param user_data    回调上下文
 * @param out_last     输出本批最后处理的 NLRI，可为 NULL
 * @param out_has_last 输出本批是否处理过 head，可为 NULL
 * @param out_processed 输出本批处理 head 数，可为 NULL
 * @return TRUE 表示已遍历到 RIB 末尾，FALSE 表示仍有未处理 head 或被回调提前停止
 */
gboolean bgp_rib_walk_heads_from(bgp_rib_t *rib, const bgp_nlri_entry_t *last_nlri, gboolean has_last, uint32_t budget,
                                 bgp_rib_head_walk_cb cb, gpointer user_data, bgp_nlri_entry_t *out_last,
                                 gboolean *out_has_last, uint32_t *out_processed);

/**
 * @brief 清理指定 NLRI 下已 stale 且未 flushed 的路由节点
 *
 * 典型场景：下刷队列完成撤销后清理 tombstone 路由，必要时连同空 rthead 一并删除。
 *
 * @param rib  目标 RIB
 * @param nlri NLRI 匹配键
 * @return 清理的路由节点数量
 */
uint32_t bgp_rib_cleanup_stale(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri);

#endif /* BGP_RIB_H */
