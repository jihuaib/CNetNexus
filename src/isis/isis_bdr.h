/**
 * @file   isis_bdr.h
 * @brief  ISIS 配置构建器（show current-configuration）
 * @author jhb
 * @date   2026/04/11
 */
#ifndef ISIS_BDR_H
#define ISIS_BDR_H

#include "dev.h"

int isis_bdr_handle_show_config(dev_ipc_message_t *msg);

#endif /* ISIS_BDR_H */
