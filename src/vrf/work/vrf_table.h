/**
 * @file   vrf_table.h
 * @brief  VRF 内存表与 AF 状态（由 worker 线程独占访问）
 * @author jhb
 * @date   2026/05/02
 */
#ifndef VRF_TABLE_H
#define VRF_TABLE_H

#include <glib.h>
#include <stdint.h>

#include "vrf.h"

/**
 * @brief AF 维度的 RD/RT 配置
 */
typedef struct vrf_af_state
{
    uint16_t afi;
    uint8_t safi;
    uint8_t has_rd;
    vrf_rd_t rd;
    GArray *import_rts; /**< vrf_rt_t 数组 */
    GArray *export_rts;
} vrf_af_state_t;

/**
 * @brief 单条 VRF 表项
 */
typedef struct vrf_entry
{
    uint32_t vrf_id;
    char name[VRF_NAME_MAX_LEN];
    uint32_t l3vrf_table_id;
    uint8_t os_state;
    GHashTable *afs; /**< (afi<<8|safi) → vrf_af_state_t* */
} vrf_entry_t;

/**
 * @brief VRF 表
 */
typedef struct vrf_table
{
    GHashTable *by_id;   /**< uint32_t → vrf_entry_t* */
    GHashTable *by_name; /**< const char* → vrf_entry_t* */
    uint32_t next_id;    /**< 下一个 vrf_id（公网占 0） */
} vrf_table_t;

/**
 * @brief 初始化 / 销毁
 */
void vrf_table_init(vrf_table_t *t);
void vrf_table_destroy(vrf_table_t *t);

/**
 * @brief 创建 VRF（id 自动分配）。已存在返回 NULL。
 */
vrf_entry_t *vrf_table_create(vrf_table_t *t, const char *name);

/**
 * @brief 用指定 id 创建 VRF（用于 DB 恢复 / 公网占位）。已存在返回 NULL。
 */
vrf_entry_t *vrf_table_create_with_id(vrf_table_t *t, uint32_t vrf_id, const char *name, uint32_t l3vrf_table_id);

vrf_entry_t *vrf_table_find_by_id(vrf_table_t *t, uint32_t vrf_id);
vrf_entry_t *vrf_table_find_by_name(vrf_table_t *t, const char *name);

/**
 * @brief 删除 VRF（同时释放 AF/RT）
 * @return 0 成功，-1 未找到
 */
int vrf_table_delete(vrf_table_t *t, uint32_t vrf_id);

/**
 * @brief AF 操作（无事件副作用，由 cfg_apply 在调用前后驱动 pub）
 */
vrf_af_state_t *vrf_af_get_or_create(vrf_entry_t *e, uint16_t afi, uint8_t safi);
vrf_af_state_t *vrf_af_find(const vrf_entry_t *e, uint16_t afi, uint8_t safi);
int vrf_af_delete(vrf_entry_t *e, uint16_t afi, uint8_t safi);
int vrf_af_set_rd(vrf_af_state_t *af, const vrf_rd_t *rd);
int vrf_af_modify_rt(vrf_af_state_t *af, int direction, int add, const vrf_rt_t *rt);

#endif /* VRF_TABLE_H */
