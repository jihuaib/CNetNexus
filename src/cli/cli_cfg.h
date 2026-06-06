/**
 * @file   cli_cfg.h
 * @brief  CFG 模块 CLI 命令处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_CFG_H
#define CLI_CFG_H

#include "cli.h"
#include "cli_handler.h"
#include "cli_tree.h"

/** CFG CLI group_id 定义（与 commands.xml 中 group-id 一致） */
#define CLI_GROUP_ID_SHOW_COMMANDS 1   /**< show cli command-info */
#define CLI_GROUP_ID_SHOW_HISTORY 2    /**< show cli history */
#define CLI_GROUP_ID_SHOW_CONFIG 3     /**< show current-configuration */
#define CLI_GROUP_ID_EXIT 4            /**< exit */
#define CLI_GROUP_ID_CONFIG 5          /**< config */
#define CLI_GROUP_ID_END 6             /**< end */
#define CLI_GROUP_ID_SHOW_CONTEXT 8    /**< show cli context */
#define CLI_GROUP_ID_SHOW_THIS 10      /**< show this */
#define CLI_GROUP_ID_SHOW_CLIENT 11    /**< show cli client */
#define CLI_GROUP_ID_SHOW_CONF_DIFF 12 /**< show configuration difference current-configuration <configuration-file>   \
                                        */

/**
 * @brief 响应输出结构
 */
typedef struct cli_resp_out
{
    char message[CLI_MAX_RESP_LEN];
    int success;
    uint32_t has_more;     /**< 是否有更多数据 */
    uint32_t batch_offset; /**< 续传偏移量 */
} cli_resp_out_t;

/**
 * @brief CFG 模块本地 CLI 命令处理
 * @param msg 已打包的 DB payload 消息
 * @param session CLI 会话
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int cli_handle(dev_ipc_message_t *msg, cli_session_t *session);

/**
 * @brief 汇聚当前 running 配置为 BDR 文本（调用方接管 GString）
 * @param exclude_module_id 非 0 时跳过该模块；DB 内部 save 调用时用于避免 DB→CLI→DB 环路
 * @return 新分配的 GString，失败时返回空串
 */
GString *cli_cfg_collect_current_config(uint32_t exclude_module_id);

#endif // CLI_CFG_H
