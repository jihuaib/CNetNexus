/**
 * @file   ldp_worker.h
 * @brief  LDP worker 线程：协议状态机、定时器与 show 处理（M1 占位）
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_WORKER_H
#define LDP_WORKER_H

#include "dev.h"

int ldp_worker_prepare(void);
int ldp_worker_launch(void);
void ldp_worker_shutdown(void);

int ldp_worker_post_show_cli(dev_ipc_message_t *msg);
int ldp_worker_post_if_event(dev_ipc_message_t *msg);

#endif /* LDP_WORKER_H */
