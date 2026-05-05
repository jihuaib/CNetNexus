/**
 * @file   ldp_bdr.h
 * @brief  LDP show current-configuration 输出
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_BDR_H
#define LDP_BDR_H

#include "dev.h"

int ldp_bdr_handle_show_config(dev_ipc_message_t *msg);

#endif /* LDP_BDR_H */
