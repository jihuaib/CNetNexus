/**
 * @file   vrf_main.c
 * @brief  VRF 模块主入口：IPC 线程 + 三阶段生命周期 + 消息分发
 * @author jhb
 * @date   2026/03/05
 */
#include "vrf_main.h"

#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "vrf_bdr.h"
#include "vrf_cli.h"
#include "vrf_db.h"
#include "work/vrf_worker.h"

vrf_local_t *g_vrf_local = NULL;

// ============================================================================
// 三阶段
// ============================================================================

/**
 * 等 DB 就绪 + 建表。
 *
 * DB 快照恢复需要通过 worker apply 回放到内存/OS，所以必须等 worker 启动后再做。
 */
static int vrf_init_db_schema(void)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();

    if (dev_ipc_wait_module_ready(ctx, DEV_MODULE_ID_DB, 5000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("VRF: DB not ready, skip db init");
        return -1;
    }
    if (vrf_db_init() != 0)
    {
        LOG_ERROR("VRF: db init failed");
        return -1;
    }
    return 0;
}

static void vrf_restore_db_state(void)
{
    if (vrf_db_load_snapshot() != 0)
    {
        LOG_WARN("VRF: db_load_snapshot failed");
    }
}

// ============================================================================
// IPC 消息分发
// ============================================================================

void vrf_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    switch (msg->msg_type)
    {
        case CLI_MSG_TYPE_SHOW_CONFIG:
            vrf_bdr_show_config(msg);
            dev_ipc_message_free(msg);
            return;

        case CLI_MSG_TYPE:
        {
            uint8_t flags = 0;
            if (msg->payload && msg->payload_len >= 1)
            {
                flags = ((const uint8_t *)msg->payload)[0];
            }
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (vrf_worker_post_ipc_message(msg) != ERRCODE_SUCCESS)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                /* DB 不可用时直接拒绝配置下发，避免内存/OS 与 DB 静默偏移 */
                if (db_rpc_guard_reject(ctx, msg, "VRF"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
                vrf_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }

        case CLI_MSG_TYPE_CONTINUE:
        case CLI_MSG_TYPE_QUERY_CANDIDATES:
        case VRF_MSG_TYPE_SUBSCRIBE:
        case VRF_MSG_TYPE_UNSUBSCRIBE:
            if (vrf_worker_post_ipc_message(msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;

        default:
            LOG_WARN("VRF: unknown msg type 0x%08X", msg->msg_type);
            dev_ipc_message_free(msg);
            return;
    }
}

// ============================================================================
// 模块初始化 / 清理
// ============================================================================

int vrf_module_init(void)
{
    log_set_tag("vrf");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_VRF, "vrf", DEV_MODULE_PORT_VRF, vrf_msg_handler);
    if (!ctx)
    {
        return -1;
    }
    g_vrf_local = g_malloc0(sizeof(*g_vrf_local));
    g_vrf_local->dev_ipc_ctx = ctx;

    /* 弱依赖模型启动：
     *   1. 等 DEV 控制连接
     *   2. wait_module_ready(DB) + db_init
     *   3. worker 启动
     *   4. db_restore 回放配置到 worker + OS
     *   5. subscribe(CLI) 放最后：CFG 看到 is_connected(VRF)=true 时本模块已 fully ready
     *   6. notify_ready */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, 10000) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("VRF: timed out waiting for DEV connection; module may be unusable");
    }

    (void)vrf_init_db_schema();

    if (vrf_worker_prepare() != ERRCODE_SUCCESS || vrf_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("VRF: worker start failed");
        vrf_worker_shutdown();
        return -1;
    }

    vrf_restore_db_state();

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("VRF: subscribe(CLI) failed");
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("VRF: notify_ready to DEV failed");
    }
    LOG_INFO("VRF: module ready");

    return 0;
}

void vrf_module_cleanup(void)
{
    vrf_worker_shutdown();

    if (!g_vrf_local)
    {
        return;
    }
    dev_ipc_context_t *ctx = g_vrf_local->dev_ipc_ctx;
    g_vrf_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }
    g_free(g_vrf_local);
    g_vrf_local = NULL;
}
