/**
 * @file   snmp_bdr.h
 * @brief  SNMP configuration export
 */
#ifndef SNMP_BDR_H
#define SNMP_BDR_H

#include "dev.h"

int snmp_bdr_handle_show_config(dev_ipc_message_t *msg);

#endif /* SNMP_BDR_H */
