/**
 * @file   cli_config_model.h
 * @brief  独立的 BDR 层级配置模型与上下文差异接口
 */
#ifndef CLI_CONFIG_MODEL_H
#define CLI_CONFIG_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/** BDR 根节点的 depth 为 0，允许的最大 depth 为 7。 */
#define CLI_CONFIG_MODEL_MAX_DEPTH 7u

/**
 * CLI 命令（不含前导缩进和行尾）的最大缓冲长度。
 * 有效命令长度必须严格小于该值。
 */
#define CLI_CONFIG_MODEL_MAX_CMD_LEN 1024u

typedef struct cli_config_node cli_config_node_t;
typedef struct cli_config_model cli_config_model_t;

/**
 * @brief 一条规范化后的 BDR 命令节点
 *
 * 所有字段均由所属 cli_config_model_t 管理，调用方只能读取，不能释放或替换。
 */
struct cli_config_node
{
    gchar *command;         /**< 去除首尾空白、内部连续空白折叠为单空格后的命令 */
    gchar *original_indent; /**< 输入行中原样保留的前导空格/Tab */
    guint original_depth;   /**< 原始前导空格/Tab 数量（每个字符计一级） */
    guint depth;            /**< 规范层级：根节点为 0 */
    guint line_number;      /**< 输入文本中的一基行号 */
    GPtrArray *children;    /**< cli_config_node_t*，保持输入顺序 */
};

/**
 * @brief 一份解析完成的 BDR 配置
 *
 * roots 中元素类型为 cli_config_node_t*，并保持输入顺序。
 */
struct cli_config_model
{
    GPtrArray *roots;
};

typedef enum cli_config_model_error
{
    CLI_CONFIG_MODEL_ERROR_INVALID_ARGUMENT = 1,
    CLI_CONFIG_MODEL_ERROR_COMMAND_TOO_LONG,
    CLI_CONFIG_MODEL_ERROR_INDENT_JUMP,
    CLI_CONFIG_MODEL_ERROR_DEPTH_EXCEEDED,
} cli_config_model_error_t;

#define CLI_CONFIG_MODEL_ERROR (cli_config_model_error_quark())

/**
 * @brief 返回配置模型错误域
 */
GQuark cli_config_model_error_quark(void);

/**
 * @brief 将 BDR 文本解析为层级模型
 *
 * 空行、去除前导空白后以 '!' 开头的行，以及命令恰为 config、exit、end
 * （大小写不敏感）的控制行不会进入模型。输入支持 LF 和 CRLF。
 *
 * 缩进中的每个空格或 Tab 表示一级；首条有效命令必须位于根层级，后续命令
 * 相对上一条有效命令最多深入一级。BDR 内容错误消息始终带一基行号；
 * API 参数错误使用“第 0 行”。
 *
 * @param text       以 NUL 结尾的 BDR 文本，不能为 NULL
 * @param out_model  成功时接收新模型；失败时为 NULL
 * @param error      可选的 GError 输出
 * @return TRUE 表示成功，FALSE 表示输入无效
 */
gboolean cli_config_model_parse(const gchar *text, cli_config_model_t **out_model, GError **error);

/**
 * @brief 释放模型及其全部节点
 */
void cli_config_model_free(cli_config_model_t *model);

/**
 * @brief 按父上下文和命令 multiset 渲染 current 到 target 的差异
 *
 * 同一父节点下的同名命令按出现次序一一配对；不同父节点之间绝不配对。
 * target 独有节点用 '+' 标记，current 独有节点用 '-' 标记。共同容器的子树
 * 存在差异时用空格标记输出一次容器上下文，使跨 VRF/AF 的同名命令仍可定位。
 * 同父节点下保持各自前序顺序；仅顺序改变不构成差异。
 *
 * 任一模型为 NULL 时按空模型处理。调用方负责用 g_string_free() 释放返回值。
 */
GString *cli_config_model_diff(const cli_config_model_t *current, const cli_config_model_t *target);

/**
 * @brief 判断两个模型是否存在上下文差异
 *
 * 语义与 cli_config_model_diff() 完全一致。
 */
gboolean cli_config_model_has_diff(const cli_config_model_t *current, const cli_config_model_t *target);

G_END_DECLS

#endif /* CLI_CONFIG_MODEL_H */
