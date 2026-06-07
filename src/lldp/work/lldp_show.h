/**
 * @file   lldp_show.h
 * @brief  LLDP show 命令
 * @author jhb
 * @date   2026/06/07
 */
#ifndef LLDP_SHOW_H
#define LLDP_SHOW_H

#include "dev.h"

int lldp_show_handle_msg(dev_ipc_message_t *msg);

#endif /* LLDP_SHOW_H */
