/**
 * @file   bgp_bulk.h
 * @brief  BGP 大批量 RIB 遍历任务：按 worker 事件分片处理并记录业务断点
 */
#ifndef BGP_BULK_H
#define BGP_BULK_H

#include <glib.h>
#include <stdint.h>

#include "bgp.h"
#include "bgp_instance.h"
#include "bgp_rd.h"
#include "bgp_rib.h"

typedef struct bgp_bulk_task bgp_bulk_task_t;

/**
 * @brief 分片 RIB head 遍历回调
 *
 * task 会在每个 slice 重新 lookup instance/RD/RIB，因此回调里的指针只在本次调用期间借用。
 */
typedef void (*bgp_bulk_inst_head_cb)(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rthead_t *head,
                                      gpointer user_data);

typedef void (*bgp_bulk_done_cb)(gpointer user_data);

/**
 * @brief 创建一个按 instance 所有 RD/RIB 遍历 head 的 bulk task
 *
 * user_data 生命周期由调用方通过 destroy 回调托管；任务完成、取消或排队失败都会调用 destroy。
 */
bgp_bulk_task_t *bgp_bulk_inst_rib_walk_create(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi,
                                               bgp_bulk_inst_head_cb head_cb, bgp_bulk_done_cb done_cb,
                                               gpointer user_data, GDestroyNotify destroy);

void bgp_bulk_task_destroy(bgp_bulk_task_t *task);

/**
 * @brief 处理一个 bulk task slice
 *
 * @return TRUE 表示任务已完成并由调用方释放，FALSE 表示任务已重新入队或仍需保留
 */
gboolean bgp_bulk_task_handle(bgp_bulk_task_t *task);

#endif /* BGP_BULK_H */
