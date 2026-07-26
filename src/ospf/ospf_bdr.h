/**
 * @file   ospf_bdr.h
 * @brief  OSPFv2 running-configuration builder
 */
#ifndef OSPF_BDR_H
#define OSPF_BDR_H

#include "dev.h"

int ospf_bdr_handle_show_config(dev_ipc_message_t *msg);
int ospf_bdr_handle_continue(dev_ipc_message_t *msg);
gboolean ospf_bdr_stream_active(void);
void ospf_bdr_cleanup(void);

#endif /* OSPF_BDR_H */
