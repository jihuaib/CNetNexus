/**
 * @file   vrf_db.h
 * @brief  VRF 持久化（vrf_instance / vrf_af / vrf_rt 三表）
 * @author jhb
 * @date   2026/05/02
 */
#ifndef VRF_DB_H
#define VRF_DB_H

#include <stdint.h>

#include "vrf.h"

/** VRF 实例表 */
#define VRF_TABLE_INSTANCE "vrf_instance"
/** VRF AF 配置表（含 RD） */
#define VRF_TABLE_AF "vrf_af"
/** VRF RT 表（每条 RT 一行） */
#define VRF_TABLE_RT "vrf_rt"

/**
 * @brief 建表（已存在则幂等跳过）。在 IPC 线程 Phase3 调用。
 */
int vrf_db_init(void);

/**
 * @brief 从数据库装载快照到 worker 的内存表（worker 线程调用）
 */
int vrf_db_load_snapshot(void);

/**
 * @brief 写入 / 更新 VRF 实例
 */
int vrf_db_insert_vrf(uint32_t vrf_id, const char *name, uint32_t l3vrf_table_id);

/**
 * @brief 删除 VRF（同时清空对应 AF / RT）
 */
int vrf_db_delete_vrf(uint32_t vrf_id);

/**
 * @brief 写入 / 更新 (vrf_id, afi, safi) 的 RD（rd=NULL 表示清除）
 */
int vrf_db_set_af_rd(uint32_t vrf_id, uint16_t afi, uint8_t safi, const vrf_rd_t *rd);

/**
 * @brief 写入 / 更新 (vrf_id, afi, safi) 的 apply-label 模式（VRF_APPLY_LABEL_*）
 */
int vrf_db_set_af_apply_label(uint32_t vrf_id, uint16_t afi, uint8_t safi, uint8_t mode);

/**
 * @brief 删除一条 AF 配置（含级联 RT）
 */
int vrf_db_delete_af(uint32_t vrf_id, uint16_t afi, uint8_t safi);

/**
 * @brief 增删一条 RT
 * @param direction 0=import, 1=export
 * @param add       1=插入，0=删除
 */
int vrf_db_modify_rt(uint32_t vrf_id, uint16_t afi, uint8_t safi, int direction, int add, const vrf_rt_t *rt);

#endif /* VRF_DB_H */
