/**
 * @file   bgp_apply_vrf.h
 * @brief  BGP 应用 VRF 事件（worker 线程）：缓存联动 + RD entry 联动
 * @author jhb
 * @date   2026/05/02
 */
#ifndef BGP_APPLY_VRF_H
#define BGP_APPLY_VRF_H

#include "dev.h"

/**
 * @brief 处理一条 VRF_MSG_TYPE_EVENT 通知
 *
 * 1. 调用 vrf_api_cache_on_event() 维护本地缓存
 * 2. 联动 BGP 内部状态：RD 新增触发 bgp_protocol_ensure_rd_entry，
 *    RD 删除/VRF AF 删除清理 BGP VRF AF，VRF 删除等价于销毁 bgp_vrf（若已存在）
 *
 * 调用方仍持有 msg 所有权，函数不释放。
 */
void bgp_apply_vrf_event(const dev_ipc_message_t *msg);

/**
 * @brief 拆除所有非 public 的 bgp_vrf_t（保留 public VRF）。
 *
 * VRF 进程重启时由 SMOOTHSTART 触发：清掉所有依赖被重启 VRF 的内存业务
 * （sessions / instances / neighbors 由 bgp_vrf_destroy 级联释放）。
 * 不动 DB，等 SMOOTHEND 后从 DB 重恢复。
 */
void bgp_apply_vrf_purge_non_public(void);

/**
 * @brief worker 周期 tick：重试 VRF/AF 删除时失败的 SRv6 SID release，
 *        并在成功后完成原子性延后的 AF/VRF 回收。
 */
void bgp_apply_vrf_cleanup_retry_tick(void);

#endif /* BGP_APPLY_VRF_H */
