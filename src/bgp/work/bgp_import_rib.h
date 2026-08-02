/**
 * @file   bgp_import_rib.h
 * @brief  BGP 跨 AF 路由互导（import-rib）：把 labeled/VPN RIB 的最优路由
 *         镜像到 unicast RIB，由 unicast 走标准优选/下刷路径进 ROUTE 模块。
 * @author jhb
 * @date   2026/05/17
 *
 * 设计要点：
 *  - 标签 AF 的 instance 不再向 ROUTE 模块下刷（避免与单播路由混淆）
 *  - unicast AF 通过命令 `import-rib labeled-unicast` 引入对应 labeled RIB 的最优
 *  - mirror 节点（unicast RIB 内）通过 src_route 反向指源节点，源节点维护 refcnt
 *  - 同 prefix 下 IP 迭代（非 mirror）优于隧道迭代（mirror）
 *
 * 钩子约定：
 *  - 已有文件（bgp_calc/bgp_route_flush/bgp_route_node_free/...）仅调用本文件 API，
 *    所有 mirror 拷贝、refcount、队列、隧道注册、级联 free 等逻辑都在 bgp_import_rib.c 内。
 */
#ifndef BGP_IMPORT_RIB_H
#define BGP_IMPORT_RIB_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "bgp.h"
#include "bgp_instance.h"
#include "bgp_rib.h"

struct dev_ipc_context;
typedef struct dev_ipc_context dev_ipc_context_t;

/**
 * @brief import-rib 源类型位掩码值（用于 inst->import_rib_sources）
 */
typedef enum bgp_import_src
{
    BGP_IMPORT_SRC_LABELED_UC = 0, /**< 同 VRF 同 AFI 的 labeled-unicast → unicast */
    BGP_IMPORT_SRC_VPN_UC = 1,     /**< 预留：vpnv4/vpnv6 → 公网 unicast */
    BGP_IMPORT_SRC_VPN_INST = 2,   /**< 预留：私网 VRF → 公网 VRF */
} bgp_import_src_t;

/**
 * @brief 模块级初始化（在 BGP worker 启动早期调用一次）
 * @return 0 成功
 */
int bgp_import_rib_init(void);

/**
 * @brief 为单个 instance 初始化 import-rib 内部状态（pending queue、反向哈希等）
 *
 * 由 bgp_instance_create() 末尾调用。labeled instance 在此一并标记 no_route_flush。
 */
void bgp_import_rib_inst_init(bgp_instance_t *inst);

/**
 * @brief 销毁 instance 的 import-rib 内部状态
 *
 * 由 bgp_instance_destroy() 调用。会撤销该 instance 名下所有 mirror、释放 pending queue。
 */
void bgp_import_rib_inst_destroy(bgp_instance_t *inst);

/**
 * @brief labeled instance 是否应跳过 ROUTE 模块下刷
 *
 * 由 bgp_route_flush_queue_process() 顶部调用。labeled 直接跳过，路由停留 BGP RIB。
 */
bool bgp_import_rib_should_skip_flush(const bgp_instance_t *inst);

/** 只读返回实例尚未处理的 import-rib 队列长度。 */
uint32_t bgp_import_rib_pending_count(const bgp_instance_t *inst);

/**
 * @brief 判定一条 route_node 是否为 mirror（即 import-rib 镜像）
 */
bool bgp_import_rib_is_mirror(const bgp_route_node_t *route);

/**
 * @brief 优选 tiebreaker：相同 prefix 下 IP 迭代优于隧道迭代（mirror）
 *
 * 由 route_is_better() 调用。
 * @return  -1 = candidate 更差，1 = candidate 更好，0 = 不区分（继续后续比较）
 */
int bgp_import_rib_tiebreak(const bgp_route_node_t *cand, const bgp_route_node_t *cur);

/**
 * @brief labeled calc 完成后通知：src_inst 的 head 上 best 发生变化
 *
 * 由 bgp_calc_route_select() 末尾调用。若同 VRF 同 AFI 的 unicast inst 设了
 * import-rib labeled-unicast，则将 head 推入对应 unicast inst 的 pending queue。
 */
void bgp_import_rib_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head, const bgp_route_node_t *old_best,
                                 const bgp_route_node_t *new_best);

/**
 * @brief flush 时为 mirror 路由触发隧道迭代并下刷 ROUTE
 *
 * 由 bgp_route_flush_queue_process() 在 mirror 分支调用。
 * @return 0 成功
 */
int bgp_import_rib_flush_mirror(dev_ipc_context_t *ctx, uint32_t vrf_id, const bgp_nlri_entry_t *nlri,
                                bgp_route_node_t *mirror, bool withdraw);

/**
 * @brief 开启 unicast inst 的某个 import 源（CLI/DB restore 入口）
 *
 * 开启后扫源 RIB 全量灌入 unicast pending queue。
 * @return 0 成功
 */
int bgp_import_rib_enable(bgp_instance_t *inst, bgp_import_src_t src);

/**
 * @brief 关闭 unicast inst 的某个 import 源
 *
 * 关闭时清空该源对应的 mirror 集合，逐条走 withdraw 路径。
 * @return 0 成功
 */
int bgp_import_rib_disable(bgp_instance_t *inst, bgp_import_src_t src);

/**
 * @brief 批量处理 inst 的 import_pending 队列（每次最多 batch 条）
 *
 * 由 worker 主循环按节奏调用。
 * @return 实际处理条目数
 */
int bgp_import_rib_queue_process(bgp_instance_t *inst, int batch);

/**
 * @brief labeled calc 批次处理完毕后顺手驱动 unicast 侧 import_pending
 *
 * 由 bgp_calc_queue_process 末尾一次调用，无副作用幂等。
 */
void bgp_import_rib_drain_after_calc(bgp_instance_t *src_inst);

/**
 * @brief 同步抽干 inst 的 import_pending 队列（drain_pending 路径）
 * @return 实际处理条目数
 */
int bgp_import_rib_process_pending(bgp_instance_t *inst);

/* ------------------------------------------------------------------------- */
/* cfg_apply orchestrator：由 bgp_cmd.c 在 dispatch 时调用                    */
/* 实现位于 bgp_import_rib.c，对接 bgp_apply_cmd_t.u.import_rib 输入字段。   */
/* ------------------------------------------------------------------------- */
struct bgp_apply_cmd;
void bgp_cfg_apply_import_rib(struct bgp_apply_cmd *apply);

#endif /* BGP_IMPORT_RIB_H */
