/**
 * @file   route_worker.h
 * @brief  Route worker 线程：epoll 事件循环与业务命令队列
 * @author jhb
 * @date   2026/03/28
 */
#ifndef ROUTE_WORKER_H
#define ROUTE_WORKER_H

#include <glib.h>
#include <pthread.h>
#include <stdint.h>

#include "dev.h"
#include "route_cfg_apply.h"
#include "route_pub.h"
#include "route_rib.h"
#include "route_work.h"

// ============================================================================
// 命令类型
// ============================================================================

/**
 * @brief worker 命令类型
 */
typedef enum route_worker_cmd_type
{
    ROUTE_WORKER_CMD_INJECT = 1,        /**< 路由注入/撤销（ROUTE_MSG_TYPE_INJECT） */
    ROUTE_WORKER_CMD_NH_REGISTER = 2,   /**< nexthop 注册（ROUTE_MSG_TYPE_NH_REGISTER） */
    ROUTE_WORKER_CMD_NH_UNREGISTER = 3, /**< nexthop 取消注册（ROUTE_MSG_TYPE_NH_UNREGISTER） */
    ROUTE_WORKER_CMD_NHOBJ_ACQUIRE = 4, /**< nexthop 对象申请（ROUTE_MSG_TYPE_NHOBJ_ACQUIRE） */
    ROUTE_WORKER_CMD_NHOBJ_RELEASE = 5, /**< nexthop 对象释放（ROUTE_MSG_TYPE_NHOBJ_RELEASE） */
    ROUTE_WORKER_CMD_SUBSCRIBE = 6,     /**< 路由订阅（ROUTE_MSG_TYPE_SUBSCRIBE） */
    ROUTE_WORKER_CMD_UNSUBSCRIBE = 7,   /**< 取消订阅（ROUTE_MSG_TYPE_UNSUBSCRIBE） */
    ROUTE_WORKER_CMD_CLI_SHOW = 8,      /**< show 命令（CLI_MSG_TYPE with SHOW_CMD flag / CLI_MSG_TYPE_CONTINUE） */
    ROUTE_WORKER_CMD_APPLY = 9,         /**< 配置应用命令（waitable，IPC 线程同步等待结果） */
    ROUTE_WORKER_CMD_SHUTDOWN = 10,     /**< 停止 worker 线程循环 */
    ROUTE_WORKER_CMD_IF_EVENT = 11,     /**< IF 接口事件（UP/DOWN/ADDR_ADD/ADDR_DEL） */
    ROUTE_WORKER_CMD_FIB_ROUTE_RESULT = 12, /**< FIB route 下发结果事件 */
    ROUTE_WORKER_CMD_VRF_EVENT = 13,        /**< VRF 事件：维护 worker 独占 VRF cache */
    ROUTE_WORKER_CMD_VRF_QUERY = 14,        /**< VRF 查询：其他线程同步请求 worker 查询 */
    ROUTE_WORKER_CMD_IF_DOWN = 15,          /**< IF 模块 DOWN：清 IF 缓存 + 重算 nexthop watch */
    ROUTE_WORKER_CMD_VRF_DOWN = 16,         /**< VRF 模块 DOWN：拆非 public VRF 业务 + 清 cache */
} route_worker_cmd_type_t;

// ============================================================================
// worker 本地状态（worker 线程专用业务数据）
// ============================================================================

/**
 * @brief 批量路由追踪条目（内存 RIB + DB route_batch 表，以 name 为主键）
 */
typedef struct route_batch_entry
{
    char name[64];                        /**< batch 名称（DB 主键） */
    uint32_t vrf_id;                      /**< VRF ID */
    uint16_t afi;                         /**< 地址族 */
    uint8_t prefix_len;                   /**< 前缀长度 */
    uint8_t _pad;                         /**< 填充对齐 */
    int32_t metric;                       /**< 度量值 */
    int32_t preference;                   /**< 管理距离 */
    net_addr_t prefix_addr;               /**< 前缀地址（二进制） */
    net_addr_t nexthop_addr;              /**< 下一跳地址（二进制） */
    char out_ifname[IF_LOGICAL_NAME_MAX]; /**< 出接口逻辑名（空字符串=不约束） */
} route_batch_entry_t;

/**
 * @brief Route worker 线程本地状态
 *
 * 包含两类数据：
 * - 业务数据（rib / subscribers / batch_entries）：由 route_worker_prepare() 初始化，
 *   worker 线程独占访问（restore 通过 route_worker_dispatch_apply() 同步派发），
 *   由 route_worker_shutdown() 负责销毁。
 * - Worker 基础设施（epoll / eventfd / 队列）：由 route_worker_prepare() 初始化，
 *   由 route_worker_shutdown() 负责销毁。
 */
typedef struct route_work_local
{
    /* ---- 业务数据（worker 线程独占） ---- */
    route_rib_t *rib;     /**< 内存路由信息库 */
    GList *subscribers;   /**< 订阅者列表 GList<route_subscriber_t*> */
    GList *batch_entries; /**< 批量路由条目 GList<route_batch_entry_t*> */

    /* ---- Worker 基础设施 ---- */
    int epoll_fd;            /**< worker epoll fd */
    volatile int running;    /**< worker 线程运行标志 */
    pthread_t thread;        /**< worker 线程句柄 */
    int cmd_eventfd;         /**< 命令唤醒 eventfd，-1 表示未创建 */
    GAsyncQueue *cmd_queue;  /**< 命令队列 */
    int work_eventfd;        /**< 工作事件唤醒 eventfd，-1 表示未创建 */
    GAsyncQueue *work_queue; /**< 工作事件队列 */
} route_work_local_t;

extern route_work_local_t *g_route_work_local;

// ============================================================================
// API
// ============================================================================

/**
 * @brief 初始化 worker 生命周期资源（业务数据 + epoll/eventfd/队列），不启动线程
 *
 * @return 0 成功，-1 失败
 */
int route_worker_prepare(void);

/**
 * @brief 启动 worker 线程（须在 route_worker_prepare 之后调用）
 * @return 0 成功，-1 失败
 */
int route_worker_launch(void);

/**
 * @brief 向 worker 线程投递异步命令（msg 所有权转移）
 *
 * @param type 命令类型
 * @param msg  关联 IPC 消息（可为 NULL）；所有权转移给 worker
 * @return 0 成功，-1 失败
 */
int route_worker_post(route_worker_cmd_type_t type, dev_ipc_message_t *msg);

/**
 * @brief 向 worker 线程异步投递 show 命令（msg 所有权转移）
 *
 * @param msg 关联 IPC 消息；所有权转移给 worker
 * @return 0 成功，-1 失败
 */
int route_worker_post_show_cli(dev_ipc_message_t *msg);

/**
 * @brief 向 worker 线程同步派发配置应用命令（阻塞直到 worker 完成）
 *
 * @param apply 配置应用命令（栈分配，由调用者持有生命周期）
 * @return 0 成功，-1 失败
 */
int route_worker_dispatch_apply(route_apply_cmd_t *apply);

/**
 * @brief 向 worker 同步投递 VRF 事件（msg 所有权转移）
 *
 * VRF 客户端缓存由 route worker 线程独占维护。IPC 线程收到 VRF_MSG_TYPE_EVENT 后
 * 必须通过本接口让 worker 更新缓存。
 *
 * @param msg VRF 事件消息；所有权转移给 worker
 * @return 0 成功，-1 失败
 */
int route_worker_dispatch_vrf_event(dev_ipc_message_t *msg);

/**
 * @brief worker 线程内：拆除所有非 public VRF 在 RIB 中的静态路由（保留 DB）。
 *        仅在 VRF SMOOTHSTART 路径上调用，用于 VRF 进程重启的业务清理。
 */
void route_worker_purge_non_public_vrf_business(void);

/**
 * @brief 通过 worker 线程按 VRF 名称解析 vrf_id
 *
 * @param vrf_name VRF 名称；NULL/空/public 解析为默认 VRF
 * @param vrf_id   输出 VRF ID
 * @return ERRCODE_SUCCESS 成功，否则失败
 */
int route_worker_resolve_vrf_id_by_name(const char *vrf_name, uint32_t *vrf_id);

/**
 * @brief 关闭 worker 线程并释放所有 worker 资源（含 rib / subscribers / batch_entries）
 */
void route_worker_shutdown(void);

/**
 * @brief 统一入口（按 nexthop_id）：发布方已预先申请 nexthop 对象（并写好 relay），下发 route 只带 id。
 *
 *
 * @param nexthop_id  已申请的 nexthop 对象 id（非 0）
 * @param out_ifindex 原始(配置)出接口（进入路径；是否参与 nexthop 身份由发布方申请对象时决定）
 * @return <0 失败；0 表示更新已有路径；>0 表示新增路径
 */
int route_add_and_notify_nexthop_id(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                                    uint32_t protocol, const net_addr_t *source_addr, uint32_t nexthop_id,
                                    int32_t metric, int32_t preference, uint32_t out_ifindex);

/**
 * @brief 全量重算"已注册 nexthop watch"的可达性，并按状态变化回推
 */
void route_recompute_iter_paths(void);

#endif /* ROUTE_WORKER_H */
