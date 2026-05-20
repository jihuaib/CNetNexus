/**
 * @file   bgp_db_internal.h
 * @brief  BGP DB 模块内部共享：表 schema、PK 构造器、共享 helper
 * @author jhb
 * @date   2026/04/26
 *
 * 仅 src/bgp/db 目录下的 .c 与 src/bgp/bgp_db.c 使用，不对外暴露。
 * 公开 API 仍声明在 include/bgp_db.h 中。
 */

#ifndef BGP_DB_INTERNAL_H
#define BGP_DB_INTERNAL_H

#include "bgp.h"
#include "bgp_cli.h"
#include "bgp_db.h"
#include "db.h"
#include "dev.h"

// ============================================================================
// 各表 schema（定义在对应 bgp_db_<table>.c，统一在 bgp_db_init 中建表）
// ============================================================================

extern const db_table_def_t BGP_PROTOCOL_TABLE;
extern const db_table_def_t BGP_SESSION_TABLE;
extern const db_table_def_t BGP_NEIGHBOR_TABLE;
extern const db_table_def_t BGP_INSTANCE_TABLE;
extern const db_table_def_t BGP_QP_ROUTE_TABLE;
extern const db_table_def_t BGP_VRF_TABLE;

// ============================================================================
// PK 构造器（每个调用点：init → add_* → 用 .filter → 必须 db_filter_clear）
// ============================================================================

/** session 表 PK：vrf_name + neighbor_ip */
void bgp_db_session_pk(db_filter_builder_t *pk, const char *vrf_name, const char *neighbor_ip);

/** neighbor 表 PK：vrf_name + afi + safi + neighbor_ip */
void bgp_db_neighbor_pk(db_filter_builder_t *pk, const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi,
                        const char *neighbor_ip);

/** vrf 表 PK：vrf_name */
void bgp_db_vrf_pk(db_filter_builder_t *pk, const char *vrf_name);

/** instance 表 PK：vrf_name + afi + safi */
void bgp_db_instance_pk(db_filter_builder_t *pk, const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi);

/** qp_route 表 PK：vrf_name + afi + safi + start_dqpn + route_count + prefix_addr + mask_len + bid */
void bgp_db_qp_route_pk(db_filter_builder_t *pk, const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi,
                        uint32_t start_dqpn, uint32_t count, const char *prefix_addr, uint8_t mask_len,
                        const char *bid);

// ============================================================================
// 共享 helper
// ============================================================================

/**
 * @brief 清空整张表（无 WHERE 条件），并把删除行数写入 *rows_out
 * @return 0 成功，-1 失败
 */
int bgp_db_delete_table_all(dev_ipc_context_t *ctx, const char *table_name, int *rows_out);

// ============================================================================
// 各表的启动恢复入口（由 bgp_db_restore 顺序调用）
// ============================================================================

uint32_t bgp_db_restore_protocol(void);
void bgp_db_restore_vrf(void);
void bgp_db_restore_sessions(void);
void bgp_db_restore_instances(void);
void bgp_db_restore_neighbors(void);
void bgp_db_restore_qp_routes(void);
void bgp_db_restore_qp_route_select(void);

#endif /* BGP_DB_INTERNAL_H */
