/**
 * @file   ldp_db_internal.h
 * @brief  LDP DB 模块内部共享：表 schema、PK 构造器
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_DB_INTERNAL_H
#define LDP_DB_INTERNAL_H

#include "db.h"
#include "dev.h"
#include "ldp.h"
#include "ldp_db.h"
#include "ldp_main.h"

extern const db_table_def_t LDP_PROTO_TABLE;
extern const db_table_def_t LDP_IF_TABLE;

void ldp_db_proto_pk(db_filter_builder_t *pk);
void ldp_db_if_pk(db_filter_builder_t *pk, const char *ifname);
int ldp_db_ensure_proto_row(dev_ipc_context_t *ctx);
int ldp_db_sync_revive_marker(dev_ipc_context_t *ctx);

void ldp_db_restore_proto(void);
void ldp_db_restore_interfaces(void);

#endif /* LDP_DB_INTERNAL_H */
