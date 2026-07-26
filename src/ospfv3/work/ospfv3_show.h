/**
 * @file   ospfv3_show.h
 * @brief  OSPFv3 operational CLI
 */
#ifndef OSPFV3_SHOW_H
#define OSPFV3_SHOW_H

#include "dev.h"

int ospfv3_show_handle_msg(dev_ipc_message_t *msg);
void ospfv3_show_cleanup(void);

#endif /* OSPFV3_SHOW_H */
