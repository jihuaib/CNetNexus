/**
 * @file   bgp_cli.h
 * @brief  BGP 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef BGP_CLI_H
#define BGP_CLI_H

#include "cli.h"
#include "dev.h"

/** BGP CLI group_id 定义（与 commands.xml 中 group-id 一致） */
#define BGP_CLI_GROUP_ID_PROTOCOL 1        /**< bgp 协议配置命令 */
#define BGP_CLI_GROUP_ID_NEIGHBOR 2        /**< neighbor 会话配置命令 */
#define BGP_CLI_GROUP_ID_ADDR_FAMILY 3     /**< address-family 进入子视图 */
#define BGP_CLI_GROUP_ID_AF_NEIGHBOR 4     /**< 地址族 neighbor enable 命令 */
#define BGP_CLI_GROUP_ID_ROUTER_ID 5       /**< router-id 配置命令 */
#define BGP_CLI_GROUP_ID_CONNECT_RETRY 6   /**< timer connect-retry 配置命令 */
#define BGP_CLI_GROUP_ID_TIMERS 7          /**< timer keepalive/hold 配置命令 */
#define BGP_CLI_GROUP_ID_OPEN_CAP 8        /**< neighbor open-capability 配置命令 */
#define BGP_CLI_GROUP_ID_SHOW_NEIGHBOR 9   /**< show bgp neighbor 详情命令 */
#define BGP_CLI_GROUP_ID_SHOW_ROUTE 10     /**< show bgp route af-* 路由显示命令 */
#define BGP_CLI_GROUP_ID_IMPORT_ROUTE 11   /**< import-route 协议导入命令 */
#define BGP_CLI_GROUP_ID_SOURCE_IF 12      /**< neighbor source-interface 配置命令 */
#define BGP_CLI_GROUP_ID_EBGP_MULTIHOP 13  /**< neighbor ebgp-multihop 配置命令 */
#define BGP_CLI_GROUP_ID_SHOW_ATTR 14      /**< show bgp attr 属性详情命令 */
#define BGP_CLI_GROUP_ID_SHOW_UG 15        /**< show bgp update-group 打包组详情命令 */
#define BGP_CLI_GROUP_ID_QP_ROUTE 16       /**< QP 自产生路由配置 */
#define BGP_CLI_GROUP_ID_ROUTE_SELECT 17   /**< QP 地址族 route-select 开关 */
#define BGP_CLI_GROUP_ID_REFRESH 18        /**< refresh bgp 路由刷新命令（RFC 2918） */
#define BGP_CLI_GROUP_ID_CLUSTER_ID 20     /**< reflector cluster-id 配置命令 */
#define BGP_CLI_GROUP_ID_REFLECT_CLIENT 21 /**< neighbor reflect-client 配置命令（AF 视图） */

/**
 * @brief 处理配置类 CLI 命令（group 1-8, 11-13），在 IPC worker 线程调用
 *
 * 通过 bgp_worker_dispatch_apply() 将状态变更派发到 BGP worker 线程，
 * 然后在 IPC worker 线程完成 DB 写入并发送响应。
 * @param msg 消息（调用者持有所有权，调用后仍需 free）
 * @return ERRCODE_SUCCESS 成功
 */
int bgp_cli_handle_config_msg(dev_ipc_message_t *msg);

/**
 * @brief 发送单包 CLI 响应（CLI_MSG_TYPE_RESP）
 * @param msg 原始请求消息
 * @param text 响应文本（NULL 视为空串）
 */
void bgp_send_cli_response(dev_ipc_message_t *msg, const char *text);

#endif // BGP_CLI_H
