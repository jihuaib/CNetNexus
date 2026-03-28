/**
 * @file   route_calc.h
 * @brief  Route 优选模块：对同一前缀的多协议路径按管理距离选最优路径，唯一下发 OS
 * @author jhb
 * @date   2026/03/24
 */
#ifndef ROUTE_CALC_H
#define ROUTE_CALC_H

#include "dev.h"
#include "route_rib.h"

// ============================================================================
// 生命周期
// ============================================================================

/**
 * @brief 初始化优选状态（基于 route_path 的 OS_INSTALLED 标记）
 */
void route_calc_init(void);

/**
 * @brief 清理优选状态（模块关闭时调用）
 */
void route_calc_cleanup(void);

// ============================================================================
// RIB 变更通知接口
// ============================================================================

/**
 * @brief 路径新增或更新后调用，重新优选并同步 OS
 *
 * 遍历前缀头下所有路径，选 preference 最小（相等时 metric 最小）的路径：
 *   - 若优选结果与当前已安装路径相同，执行一次 install（REPLACE 语义）并发送 add 通知
 *   - 若优选结果切换到其他路径，先 withdraw 旧路由，再 install 新路由
 *
 * @param head 已更新的前缀头（新路径已写入 RIB）
 */
void route_calc_on_path_add(const route_head_t *head);

/**
 * @brief 路径删除前调用（del_path 仍在 RIB 中），重新优选并同步 OS 及订阅者
 *
 * 在 route_rib_del 的删除前回调中调用，此时 del_path 尚未从 RIB 移除。
 * 优选时自动跳过 del_path，从剩余路径中选出新最优路径：
 *   - 若 del_path 不是当前已安装路径，则不操作
 *   - 若 del_path 是当前已安装路径：通知订阅者 withdraw + OS withdraw，安装新最优路由（若剩余路径为空则仅 withdraw）
 *
 * @param head     前缀头
 * @param del_path 即将被删除的路径
 */
void route_calc_on_path_del(const route_head_t *head, const route_path_t *del_path);

/**
 * @brief 向指定模块发送当前所有最优路径快照（用于 SUBSCRIBE+FULL 响应）
 *
 * 遍历 RIB 中标记为 OS_INSTALLED 的路径，按 protocol 和 vrf_id 过滤后打包为
 * ROUTE_MSG_TYPE_REPORT 消息发送给目标模块。
 *
 * @param dst_module_id 目标模块 ID
 * @param protocol     协议过滤（ROUTE_PROTOCOL_MAX = 全部）
 * @param vrf_id       VRF 过滤（ROUTE_VRF_ALL = 全部）
 * @param request_id   原始请求 ID（用于响应配对）
 */
void route_calc_pub_dump(uint32_t dst_module_id, uint32_t protocol, uint32_t vrf_id, uint32_t request_id);

#endif /* ROUTE_CALC_H */
