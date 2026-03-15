/**
 * @file   bgp_pub.h
 * @brief  BGP 路由发布链表接口（本地起源路由的宣告与撤销管理）
 * @author jhb
 * @date   2026/03/15
 */
#ifndef BGP_PUB_H
#define BGP_PUB_H

#include <glib.h>
#include <stdint.h>

/* 包含顺序：bgp_instance.h 传递包含 bgp_peer.h（定义枚举），之后再包含 bgp.h（定义宏），避免名称冲突 */
#include "bgp.h"
#include "bgp_conn.h"
#include "bgp_instance.h"

/**
 * @brief 单条发布路由条目
 *
 * 保存本地注入 BGP 的一条路由：NLRI + 路径属性 + 下一跳。
 * 由 bgp_publist_t 持有所有权。
 */
typedef struct bgp_pub_entry
{
    bgp_nlri_entry_t nlri; /**< NLRI（含 afi/safi/type/key/prefix） */
    bgp_attr_t attr;       /**< 路径属性（ORIGIN/AS_PATH/LOCAL_PREF/MED/COMMUNITY） */
    bgp_nexthop_t nexthop; /**< 下一跳地址 */
} bgp_pub_entry_t;

/**
 * @brief BGP 路由发布链表
 *
 * 挂在 bgp_instance_t 上，维护该地址族实例的本地起源路由列表。
 * 新增/更新/删除路由时自动向实例内所有 ESTABLISHED 会话宣告或撤销。
 */
typedef struct bgp_publist
{
    GList *entries;       /**< bgp_pub_entry_t* 链表（持有所有权） */
    uint32_t count;       /**< 当前条目数 */
    bgp_instance_t *inst; /**< 所属 AF 实例（借用引用，不持有所有权） */
} bgp_publist_t;

/**
 * @brief 创建路由发布链表
 * @param inst 所属 AF 实例（借用引用）
 * @return 新建的 bgp_publist_t 指针
 */
bgp_publist_t *bgp_publist_create(bgp_instance_t *inst);

/**
 * @brief 销毁路由发布链表（释放所有条目，不发送撤销报文）
 * @param list bgp_publist_t 指针（允许为 NULL）
 */
void bgp_publist_destroy(bgp_publist_t *list);

/**
 * @brief 添加或更新一条发布路由
 *
 * 若 NLRI key 已存在则更新属性并重新宣告；否则新增并宣告。
 * 仅向该 AF 实例内（peer_hash）处于 ESTABLISHED 状态的会话发送 UPDATE。
 *
 * @param list    发布链表
 * @param nlri    NLRI 条目（深拷贝）
 * @param attr    路径属性（深拷贝）
 * @param nexthop 下一跳（深拷贝）
 * @return 0 成功，-1 参数无效
 */
int bgp_publist_add(bgp_publist_t *list, const bgp_nlri_entry_t *nlri, const bgp_attr_t *attr,
                    const bgp_nexthop_t *nexthop);

/**
 * @brief 删除一条发布路由
 *
 * 按 NLRI key 查找并移除，同时向实例内所有 ESTABLISHED 会话发送 WITHDRAW。
 *
 * @param list 发布链表
 * @param nlri NLRI 条目（仅使用 key 字段匹配）
 * @return 0 成功（已删除），-1 未找到或参数无效
 */
int bgp_publist_del(bgp_publist_t *list, const bgp_nlri_entry_t *nlri);

/**
 * @brief 向指定连接宣告所有当前发布路由
 *
 * 会话进入 ESTABLISHED 状态时调用，将所有已发布路由通过该连接发送给对端。
 *
 * @param list 发布链表
 * @param conn 目标连接（fd 必须有效）
 */
void bgp_publist_announce_to_conn(const bgp_publist_t *list, bgp_conn_t *conn);

/**
 * @brief 向指定连接撤销所有当前发布路由
 *
 * 会话即将主动关闭时调用，向对端撤销所有已发布路由。
 *
 * @param list 发布链表
 * @param conn 目标连接（fd 必须有效）
 */
void bgp_publist_withdraw_from_conn(const bgp_publist_t *list, bgp_conn_t *conn);

#endif /* BGP_PUB_H */
