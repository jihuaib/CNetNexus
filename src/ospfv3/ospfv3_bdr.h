/**
 * @file   ospfv3_bdr.h
 * @brief  OSPFv3 running-configuration builder
 */
#ifndef OSPFV3_BDR_H
#define OSPFV3_BDR_H

#include "dev.h"

int ospfv3_bdr_handle_show_config(dev_ipc_message_t *msg);
int ospfv3_bdr_handle_continue(dev_ipc_message_t *msg);
gboolean ospfv3_bdr_stream_active(void);
void ospfv3_bdr_cleanup(void);

#endif /* OSPFV3_BDR_H */
