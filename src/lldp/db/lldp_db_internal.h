/**
 * @file   lldp_db_internal.h
 * @brief  LLDP DB 内部共享定义
 * @author jhb
 * @date   2026/06/07
 */
#ifndef LLDP_DB_INTERNAL_H
#define LLDP_DB_INTERNAL_H

#include "db.h"
#include "dev.h"
#include "lldp.h"
#include "lldp_db.h"
#include "lldp_main.h"

extern const db_table_def_t LLDP_PROTO_TABLE;
extern const db_table_def_t LLDP_IF_TABLE;

void lldp_db_proto_pk(db_filter_builder_t *pk);
void lldp_db_if_pk(db_filter_builder_t *pk, const char *ifname);

void lldp_db_restore_proto(void);
void lldp_db_restore_interfaces(void);

#endif /* LLDP_DB_INTERNAL_H */
