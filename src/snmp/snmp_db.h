/**
 * @file   snmp_db.h
 * @brief  SNMP module DB-backed configuration
 */
#ifndef SNMP_DB_H
#define SNMP_DB_H

#include "db.h"
#include "snmp.h"

#define SNMP_TABLE_CONFIG "snmp_config"
#define SNMP_DB_CONFIG_NOT_FOUND 1

int snmp_db_init(void);
int snmp_db_restore(void);
/* ERRCODE_SUCCESS=row present, SNMP_DB_CONFIG_NOT_FOUND=no row, ERRCODE_FAIL=query failed. */
int snmp_db_get_config(snmp_config_msg_t *cfg);
int snmp_db_set_config(const snmp_config_msg_t *cfg);
int snmp_db_disable_trap(void);

#endif /* SNMP_DB_H */
