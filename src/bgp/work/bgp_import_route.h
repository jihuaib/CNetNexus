/**
 * @file   bgp_import_route.h
 * @brief  将 ROUTE 模块推送的路由条目导入到 BGP RIB（import-route）
 *
 * 提供 ROUTE_MSG_TYPE_UPDATE / ROUTE_MSG_TYPE_REPORT 的接收处理入口。
 * 内部遵循各 AF 实例的 import-route 协议开关与 AFI/SAFI 校验。
 */
#ifndef BGP_IMPORT_ROUTE_H
#define BGP_IMPORT_ROUTE_H

#include <stdint.h>

#include "dev.h"

/**
 * @brief 处理 ROUTE 模块推送的增量路由更新（ROUTE_MSG_TYPE_UPDATE）
 * @param msg IPC 消息（payload 为单条 route_msg_entry_t）
 * @return 实际导入条目数（0 表示被过滤/忽略）
 */
uint32_t bgp_import_route_on_update(const dev_ipc_message_t *msg);

/**
 * @brief 处理 ROUTE 模块全量路由快照（ROUTE_MSG_TYPE_REPORT）
 * @param msg IPC 消息（payload 为 route_msg_report_t + 变长 entry 数组）
 * @return 实际导入条目数
 */
uint32_t bgp_import_route_on_report(const dev_ipc_message_t *msg);

#endif /* BGP_IMPORT_ROUTE_H */
