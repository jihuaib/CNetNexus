/**
 * @file   ospf_show.h
 * @brief  OSPFv2 operational CLI
 */
#ifndef OSPF_SHOW_H
#define OSPF_SHOW_H

#include "dev.h"

int ospf_show_handle_msg(dev_ipc_message_t *msg);
void ospf_show_cleanup(void);

#endif /* OSPF_SHOW_H */
