/**
 * @file   lldp_bdr.h
 * @brief  LLDP show current-configuration 输出
 * @author jhb
 * @date   2026/06/07
 */
#ifndef LLDP_BDR_H
#define LLDP_BDR_H

#include "dev.h"

int lldp_bdr_handle_show_config(dev_ipc_message_t *msg);

#endif /* LLDP_BDR_H */
