/**
 * @file   ldp_show.h
 * @brief  LDP show 命令处理
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_SHOW_H
#define LDP_SHOW_H

#include "dev.h"

int ldp_show_handle_msg(dev_ipc_message_t *msg);
void ldp_show_cleanup(void);

#endif /* LDP_SHOW_H */
