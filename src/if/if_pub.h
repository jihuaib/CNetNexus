/**
 * @file   if_pub.h
 * @brief  IF 模块事件发布/订阅接口
 * @author jhb
 * @date   2026/03/15
 */
#ifndef IF_PUB_H
#define IF_PUB_H

#include <glib.h>
#include <stdint.h>

#include "dev.h"
#include "if_map.h"
#include "net_addr.h"

/**
 * @brief IF 事件订阅者信息
 */
typedef struct if_subscriber
{
    uint32_t module_id;    /**< 订阅模块 ID */
    uint32_t if_type_mask; /**< 订阅接口类型位图（IF_INTF_TYPE_*） */
    uint32_t event_mask;   /**< 订阅事件位图（IF_EVENT_*） */
} if_subscriber_t;

/**
 * @brief 向匹配订阅者发布接口事件
 * @param subscribers 订阅者列表（GList<if_subscriber_t*>）
 * @param entry       接口条目
 * @param if_type     本次事件接口类型（单值位）
 * @param event       本次事件（单值位）
 * @param admin_up    1=up, 0=down
 */
void if_pub_notify(GList *subscribers, const if_map_entry_t *entry, uint32_t if_type, uint32_t event, uint8_t admin_up);

/**
 * @brief 向匹配订阅者发布接口地址变更事件
 * @param subscribers 订阅者列表（GList<if_subscriber_t*>）
 * @param entry       接口条目
 * @param if_type     本次事件接口类型（单值位）
 * @param event       事件类型（IF_EVENT_ADDR_ADD 或 IF_EVENT_ADDR_DEL）
 * @param prefix      接口地址前缀
 * @param out_ifindex 接口索引
 */
void if_pub_notify_addr(GList *subscribers, const if_map_entry_t *entry, uint32_t if_type, uint32_t event,
                        const net_prefix_t *prefix, uint32_t out_ifindex);

#endif /* IF_PUB_H */
