/**
 * @file   vrf_main.h
 * @brief  VRF 模块主入口头文件（仅保存 IPC 线程上下文）
 * @author jhb
 * @date   2026/03/05
 */
#ifndef VRF_MAIN_H
#define VRF_MAIN_H

#include "dev.h"

/**
 * @brief VRF 模块 IPC 线程本地上下文
 *
 * 业务数据（vrf 表 / subscribers / show_stream）由 worker 线程独占，
 * 定义在 work/vrf_worker.h 的 vrf_work_local_t 中。
 */
typedef struct vrf_local
{
    dev_ipc_context_t *dev_ipc_ctx;
} vrf_local_t;

extern vrf_local_t *g_vrf_local;

/**
 * @brief 获取 VRF 模块本地 IPC 上下文（架构保证非空）
 */
static inline dev_ipc_context_t *vrf_local_ipc_ctx(void)
{
    return g_vrf_local->dev_ipc_ctx;
}

/**
 * @brief IPC 消息处理回调
 */
/** VRF 内部消息：DB 模块就绪，worker 线程做 db_init + db_restore */
#define VRF_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_VRF, 0xFFFB)

void vrf_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief VRF 模块初始化（由 vrf_proc.c main() 显式调用）
 */
int vrf_module_init(void);

/**
 * @brief VRF 模块清理（由 vrf_proc.c main() 退出前调用）
 */
void vrf_module_cleanup(void);

#endif /* VRF_MAIN_H */
