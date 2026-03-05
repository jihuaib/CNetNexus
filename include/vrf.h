/**
 * @file   vrf.h
 * @brief  VRF 模块公共接口：常量定义、RPC 消息类型及跨模块 API
 * @author jhb
 * @date   2026/03/05
 */
#ifndef VRF_H
#define VRF_H

#include <stddef.h>

#include "dev.h"

// ============================================================================
// VRF 常量定义
// ============================================================================

/** VRF 名称最大长度（含 null 终止符） */
#define VRF_NAME_MAX_LEN 64

/** 公网 VRF ID（系统启动时自动创建，不可删除） */
#define VRF_PUBLIC_VRF_ID 0

/** 公网 VRF 名称 */
#define VRF_PUBLIC_VRF_NAME "public"

// ============================================================================
// VRF RPC 消息类型（大类 = DEV_IPC_CATEGORY_VRF = 0x0007）
// ============================================================================

/** VRF RPC 请求：根据 VRF ID 查询 VRF 名称；payload = uint32_t vrf_id */
#define VRF_MSG_TYPE_GET_NAME DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_VRF, 0x0001)

/** VRF RPC 响应：payload = vrf_name 字符串（null 结尾），未找到则 payload_len=0 */
#define VRF_MSG_TYPE_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_VRF, 0x00FF)

// ============================================================================
// VRF RPC API（供其他模块调用）
// ============================================================================

/**
 * @brief 通过 VRF RPC 根据 VRF ID 获取 VRF 名称
 * @param ctx       调用方 IPC 上下文（需已连接到 VRF 模块）
 * @param vrf_id    VRF ID
 * @param name_out  输出 VRF 名称缓冲区
 * @param name_size 缓冲区大小（建议 VRF_NAME_MAX_LEN）
 * @return 成功返回 0，失败或未找到返回 -1
 */
int vrf_get_name(dev_ipc_context_t *ctx, uint32_t vrf_id, char *name_out, size_t name_size);

#endif /* VRF_H */
