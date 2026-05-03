/**
 * @file   vrf_pub.h
 * @brief  VRF 事件订阅与发布（worker 线程内）
 * @author jhb
 * @date   2026/05/02
 */
#ifndef VRF_PUB_H
#define VRF_PUB_H

#include <glib.h>
#include <stdint.h>

#include "dev.h"
#include "vrf.h"
#include "vrf_table.h"

typedef struct vrf_subscriber
{
    uint32_t module_id;
    uint32_t af_mask;
    uint32_t event_mask;
} vrf_subscriber_t;

/**
 * @brief 处理订阅 / 取消订阅消息（worker 线程）
 */
void vrf_pub_handle_subscribe(dev_ipc_message_t *msg);
void vrf_pub_handle_unsubscribe(dev_ipc_message_t *msg);

/**
 * @brief 八种事件通知（按 (event_mask, af_mask) 定向投递）
 */
void vrf_pub_notify_vrf_add(const vrf_entry_t *e);
void vrf_pub_notify_vrf_del(const vrf_entry_t *e);
void vrf_pub_notify_vrf_state(const vrf_entry_t *e);
void vrf_pub_notify_af_enable(const vrf_entry_t *e, uint16_t afi, uint8_t safi);
void vrf_pub_notify_af_disable(const vrf_entry_t *e, uint16_t afi, uint8_t safi);
void vrf_pub_notify_af_rd(const vrf_entry_t *e, const vrf_af_state_t *af);
void vrf_pub_notify_af_import_rt(const vrf_entry_t *e, const vrf_af_state_t *af);
void vrf_pub_notify_af_export_rt(const vrf_entry_t *e, const vrf_af_state_t *af);

#endif /* VRF_PUB_H */
