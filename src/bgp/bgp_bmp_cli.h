/**
 * @file   bgp_bmp_cli.h
 * @brief  BGP BMP 上报功能 CLI 命令处理头文件
 * @author jhb
 * @date   2026/03/29
 */
#ifndef BGP_BMP_CLI_H
#define BGP_BMP_CLI_H

#include "dev.h"

/** BMP CLI group_id 定义（与 commands.xml 中 group-id 一致） */
#define BGP_CLI_GROUP_ID_BMP_INSTANCE 14       /**< bmp instance 配置命令 */
#define BGP_CLI_GROUP_ID_BMP_COLLECTOR 15      /**< collector 配置命令 */
#define BGP_CLI_GROUP_ID_BMP_STATS_INTERVAL 16 /**< stats-report interval 配置命令 */
#define BGP_CLI_GROUP_ID_BMP_RECONNECT 17      /**< reconnect interval 配置命令 */
#define BGP_CLI_GROUP_ID_BMP_MONITOR 18        /**< monitor neighbor 配置命令 */
#define BGP_CLI_GROUP_ID_BMP_SHOW 19           /**< show bgp bmp 显示命令 */

/**
 * @brief 处理 BMP 配置类 CLI 命令（group 14-18），在 IPC worker 线程调用
 * @param msg 消息（调用者持有所有权，调用后仍需 free）
 * @return ERRCODE_SUCCESS 成功
 */
int bgp_bmp_cli_handle_config_msg(dev_ipc_message_t *msg);

#endif /* BGP_BMP_CLI_H */
