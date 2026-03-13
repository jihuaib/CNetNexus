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
#define BGP_CLI_GROUP_ID_PROTOCOL 1       /**< bgp 协议配置命令 */
#define BGP_CLI_GROUP_ID_SHOW 2           /**< show bgp 命令 */
#define BGP_CLI_GROUP_ID_NEIGHBOR 3       /**< neighbor 会话配置命令 */
#define BGP_CLI_GROUP_ID_ADDR_FAMILY 4    /**< address-family 进入子视图 */
#define BGP_CLI_GROUP_ID_AF_NEIGHBOR 5    /**< 地址族 neighbor enable 命令 */
#define BGP_CLI_GROUP_ID_ROUTER_ID 6      /**< router-id 配置命令 */
#define BGP_CLI_GROUP_ID_TIMERS 7         /**< timer keepalive/hold 配置命令 */
#define BGP_CLI_GROUP_ID_CONNECT_RETRY 8  /**< timer connect-retry 配置命令 */
#define BGP_CLI_GROUP_ID_OPEN_CAP 9       /**< neighbor open-capability 配置命令 */
#define BGP_CLI_GROUP_ID_SHOW_NEIGHBOR 10 /**< show bgp neighbor 详情命令 */

/**
 * @brief 处理来自 CFG 模块的 CLI 命令消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int bgp_cli_handle_message(dev_ipc_message_t *msg);

/**
 * @brief 处理 CLI continue 消息
 * @param msg 消息
 * @return ERRCODE_SUCCESS 成功
 */
int bgp_cli_handle_continue(dev_ipc_message_t *msg);

#endif // BGP_CLI_H
