/**
 * @file   cli_config_plan.h
 * @brief  层级配置回滚计划的预检与生成
 */
#ifndef CLI_CONFIG_PLAN_H
#define CLI_CONFIG_PLAN_H

#include <glib.h>

#include "cli_config_model.h"
#include "cli_view.h"

G_BEGIN_DECLS

typedef enum cli_config_plan_action
{
    CLI_CONFIG_PLAN_ACTION_UNDO = 1,
    CLI_CONFIG_PLAN_ACTION_ADD,
    CLI_CONFIG_PLAN_ACTION_ENTER,
    CLI_CONFIG_PLAN_ACTION_EXIT,
} cli_config_plan_action_t;

typedef struct cli_config_plan_step
{
    gchar *command;                  /**< 可直接交给 process_command() 的命令 */
    guint depth;                     /**< 在 config 视图之下的期望深度 */
    gint view_delta;                 /**< 执行后视图深度变化：-1、0、+1 */
    cli_config_plan_action_t action; /**< 内部计划步骤分类，用于执行审计 */
    guint source_line;               /**< 对应配置文件行号；框架 exit 为 0 */
} cli_config_plan_step_t;

typedef struct cli_config_plan
{
    GPtrArray *steps; /**< cli_config_plan_step_t* */
} cli_config_plan_t;

typedef enum cli_config_plan_error
{
    CLI_CONFIG_PLAN_ERROR_INVALID_ARGUMENT = 1,
    CLI_CONFIG_PLAN_ERROR_INVALID_COMMAND,
    CLI_CONFIG_PLAN_ERROR_INVALID_HIERARCHY,
    CLI_CONFIG_PLAN_ERROR_NO_INVERSE,
    CLI_CONFIG_PLAN_ERROR_INVALID_INVERSE,
} cli_config_plan_error_t;

#define CLI_CONFIG_PLAN_ERROR (cli_config_plan_error_quark())

GQuark cli_config_plan_error_quark(void);

/**
 * @brief 对 current/target 做完整的命令树预检，并生成 current -> target 计划
 *
 * 计划在返回前一次性完成全部逆命令求解；任一命令无法安全求逆时返回 FALSE，
 * 不向调用方暴露半成品计划。
 */
gboolean cli_config_plan_build(const cli_config_model_t *current, const cli_config_model_t *target,
                               cli_view_node_t *config_view, cli_view_node_t *view_root,
                               cli_tree_node_t *global_cmd_tree, cli_config_plan_t **out_plan, GError **error);

void cli_config_plan_free(cli_config_plan_t *plan);
gboolean cli_config_plan_is_empty(const cli_config_plan_t *plan);

G_END_DECLS

#endif /* CLI_CONFIG_PLAN_H */
