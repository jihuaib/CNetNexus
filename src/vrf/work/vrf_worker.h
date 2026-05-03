/**
 * @file   vrf_worker.h
 * @brief  VRF worker 线程：串行处理 IPC 业务、配置 apply、show 输出
 * @author jhb
 * @date   2026/05/02
 */
#ifndef VRF_WORKER_H
#define VRF_WORKER_H

#include <glib.h>
#include <pthread.h>
#include <stdint.h>

#include "cli.h"
#include "dev.h"
#include "vrf_cfg_apply.h"
#include "vrf_table.h"

/**
 * @brief worker 线程本地状态（业务数据由 worker 独占访问）
 */
typedef struct vrf_work_local
{
    vrf_table_t table;              /**< VRF/AF/RD/RT 内存表 */
    GList *subscribers;             /**< vrf_subscriber_t* */
    cli_chunk_stream_t show_stream; /**< CLI show 分片状态 */

    pthread_t thread;
    volatile int running;
    GAsyncQueue *cmd_queue;
} vrf_work_local_t;

extern vrf_work_local_t *g_vrf_work_local;

/**
 * @brief 准备 worker 资源（建表 + 创建队列），不启动线程
 */
int vrf_worker_prepare(void);

/**
 * @brief 启动 worker 线程
 */
int vrf_worker_launch(void);

/**
 * @brief 关闭 worker 线程并释放资源
 */
void vrf_worker_shutdown(void);

/**
 * @brief 投递 IPC 消息给 worker（msg 所有权转移）
 */
int vrf_worker_post_ipc_message(dev_ipc_message_t *msg);

/**
 * @brief 同步派发配置 apply（阻塞直到 worker 处理完毕）
 */
int vrf_worker_dispatch_apply(vrf_apply_cmd_t *apply);

/**
 * @brief 派发"DB 恢复"任务给 worker（用于 Phase3）
 */
int vrf_worker_dispatch_db_restore(void);

/**
 * @brief 给 pub 模块用的访问器
 */
dev_ipc_context_t *vrf_worker_ipc_ctx(void);
vrf_table_t *vrf_worker_table(void);
GList *vrf_worker_subscribers(void);
GList **vrf_worker_subscribers_ptr(void);
cli_chunk_stream_t *vrf_worker_show_stream(void);

#endif /* VRF_WORKER_H */
