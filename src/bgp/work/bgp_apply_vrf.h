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
 * 2. 联动 BGP 内部状态：RD 变更触发 bgp_protocol_ensure_rd_entry，
 *    VRF 删除等价于销毁 bgp_vrf（若已存在），RT 变更暂作 no-op 留作后续
 *
 * 调用方仍持有 msg 所有权，函数不释放。
 */
void bgp_apply_vrf_event(const dev_ipc_message_t *msg);

#endif /* BGP_APPLY_VRF_H */
