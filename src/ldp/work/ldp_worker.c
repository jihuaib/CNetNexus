/**
 * @file   ldp_worker.c
 * @brief  LDP worker 占位实现
 *
 * M1 阶段仅提供生命周期接口与 show 应答占位；后续 M2 起加入接口/邻居/会话状态机。
 *
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_worker.h"

#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "ldp_main.h"
#include "log.h"

int ldp_worker_prepare(void)
{
    return ERRCODE_SUCCESS;
}

int ldp_worker_launch(void)
{
    return ERRCODE_SUCCESS;
}

void ldp_worker_shutdown(void) {}

static void ldp_worker_send_show_resp(dev_ipc_message_t *req, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_LDP, req->src_module_id,
                                                     req->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(ldp_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

int ldp_worker_post_show_cli(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return -1;
    }
    /* M1：所有 show 命令暂时返回空文本占位 */
    ldp_worker_send_show_resp(msg, "");
    dev_ipc_message_free(msg);
    return 0;
}

int ldp_worker_post_if_event(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return -1;
    }
    /* M1：尚未消费 IF 事件，直接释放 */
    dev_ipc_message_free(msg);
    return 0;
}
