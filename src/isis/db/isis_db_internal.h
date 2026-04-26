/**
 * @file   isis_db_internal.h
 * @brief  ISIS DB 模块内部共享：表 schema、PK 构造器、共享 helper
 * @author jhb
 * @date   2026/04/26
 *
 * 仅 src/isis/db 目录下的 .c 与 src/isis/isis_db.c 使用，不对外暴露。
 * 公开 API 仍声明在 src/isis/isis_db.h 中。
 */

#ifndef ISIS_DB_INTERNAL_H
#define ISIS_DB_INTERNAL_H

#include "db.h"
#include "dev.h"
#include "isis.h"
#include "isis_db.h"
#include "isis_main.h"

extern const db_table_def_t ISIS_INSTANCE_TABLE;
extern const db_table_def_t ISIS_IF_TABLE;

void isis_db_instance_pk(db_filter_builder_t *pk, uint32_t tag);
void isis_db_interface_tag_pk(db_filter_builder_t *pk, uint32_t tag);
void isis_db_interface_key_pk(db_filter_builder_t *pk, uint32_t tag, const char *ifname);
void isis_db_interface_af_pk(db_filter_builder_t *pk, uint32_t tag, const char *ifname, uint16_t afi);

void isis_db_if_af_cfg_defaults(isis_if_af_cfg_t *cfg);
void isis_db_if_af_cfg_sanitize(isis_if_af_cfg_t *cfg);
void isis_db_if_cfg_defaults(isis_if_cfg_t *cfg, const char *ifname);
void isis_db_if_apply_row_to_af(db_row_t *row, isis_if_af_cfg_t *af_cfg);

void isis_db_restore_instances(void);
void isis_db_restore_interfaces(void);

#endif /* ISIS_DB_INTERNAL_H */
