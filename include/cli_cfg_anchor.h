/**
 * @file   cli_cfg_anchor.h
 * @brief  show current-configuration 通用配置锚点 (Config Anchor) 框架
 *
 * 设计目的:
 *   - 多个模块可能同时对同一"配置容器"贡献内嵌行, 例如接口、VRF、route-policy
 *     等。本框架把"容器"抽象成不透明的 anchor key, 让 CLI 聚合器按 key 合并
 *     各模块的贡献, 避免同一容器被多次重复输出。
 *   - CLI 框架对 anchor key 的含义完全不关心, 不含任何业务词汇; 业务模块之间
 *     通过约定的 key 字符串互相配合。
 *
 * 典型用法:
 *   - 属主模块 (owner, 例如 IF 对接口): 发射 header + 0..N 条 body + footer
 *   - 贡献者模块 (contributor, 例如 ISIS 对接口): 只发射 body
 *   - 与 anchor 无关的顶层配置: 用 cli_cfg_anchor_emit_global 原样追加
 *
 * @author jhb
 * @date   2026/04/17
 */

#ifndef CLI_CFG_ANCHOR_H
#define CLI_CFG_ANCHOR_H

#include <glib.h>
#include <stdint.h>

/** anchor key 字符串最大长度(含结尾 \0), key 由属主和贡献者自行约定 */
#define CLI_CFG_ANCHOR_KEY_MAX 128

/**
 * @brief 追加一段不属于任何 anchor 的顶层配置文本
 * @param out  模块响应缓冲
 * @param text 完整的可回放命令文本, 调用者自行负责两端的 "!\r\n" 分隔
 */
void cli_cfg_anchor_emit_global(GString *out, const char *text);

/**
 * @brief 属主模块声明 anchor <key> 的段头(通常含进入视图的命令)
 *        聚合器只保留第一次出现的 header; 重复声明会被忽略并记录日志。
 * @param out    模块响应缓冲
 * @param key    anchor key (<= CLI_CFG_ANCHOR_KEY_MAX)
 * @param header 段头文本, 例如 "!\r\nif GE-1\r\n"
 */
void cli_cfg_anchor_emit_header(GString *out, const char *key, const char *header);

/**
 * @brief 向 anchor <key> 追加一段内嵌行(属主与贡献者都可调用)
 *        多次贡献按发射顺序合并。
 * @param out  模块响应缓冲
 * @param key  anchor key
 * @param body 已缩进的命令行, 例如 " ip address 10.0.0.1 24\r\n"
 */
void cli_cfg_anchor_emit_body(GString *out, const char *key, const char *body);

/**
 * @brief 属主模块声明 anchor <key> 的段尾(通常为 "!\r\n")
 *        聚合器只保留第一次出现的 footer; 重复声明会被忽略并记录日志。
 * @param out    模块响应缓冲
 * @param key    anchor key
 * @param footer 段尾文本
 */
void cli_cfg_anchor_emit_footer(GString *out, const char *key, const char *footer);

/* ------------------------------------------------------------------
 * 以下为 CLI 聚合器内部协议约定, 业务模块请使用上面的 emit 函数,
 * 不要直接拼接这些字节。
 *
 * 线上协议(在 SHOW_CONFIG 响应字符串中内嵌):
 *   每段贡献由一对定界符包裹:
 *     \x01H <key>\x01\n<header 文本>\x01E\x01\n   (header)
 *     \x01B <key>\x01\n<body   文本>\x01E\x01\n   (body)
 *     \x01F <key>\x01\n<footer 文本>\x01E\x01\n   (footer)
 *   定界符之外的所有字节都按顺序追加到"全局片段"。
 *   \x01 (SOH) 在正常 CLI 可打印文本中不会出现, 因此可作为安全转义。
 * ------------------------------------------------------------------ */

/** 定界符字节 */
#define CLI_CFG_ANCHOR_DELIM '\x01'

/** 段类型: 属主段头 */
#define CLI_CFG_ANCHOR_KIND_HEADER 'H'
/** 段类型: 内嵌贡献 */
#define CLI_CFG_ANCHOR_KIND_BODY 'B'
/** 段类型: 属主段尾 */
#define CLI_CFG_ANCHOR_KIND_FOOTER 'F'
/** 段结束标志 */
#define CLI_CFG_ANCHOR_KIND_END 'E'

/**
 * @brief 配置锚点聚合器(CLI 内部使用)
 */
typedef struct cli_cfg_anchor_aggregator cli_cfg_anchor_aggregator_t;

/**
 * @brief 创建聚合器
 */
cli_cfg_anchor_aggregator_t *cli_cfg_anchor_agg_new(void);

/**
 * @brief 将一段来自某模块的 SHOW_CONFIG 响应文本喂入聚合器
 *        线性扫描并分发到全局缓冲或各 anchor 分桶。
 */
void cli_cfg_anchor_agg_feed(cli_cfg_anchor_aggregator_t *agg, const char *module_output);

/**
 * @brief 将聚合结果渲染到 out: 先全局片段, 再按 anchor 首次出现顺序输出
 *        (header + 合并后的 body + footer)。
 *        - 若某 anchor 无 header: 视为孤儿, 丢弃其 body 并记录日志。
 *        - 若某 anchor 无 footer: 使用默认 "!\r\n" 作为保底段尾。
 *        - 若某 anchor 的合并 body 为空: 整段跳过, 不输出空壳块。
 */
void cli_cfg_anchor_agg_render(cli_cfg_anchor_aggregator_t *agg, GString *out);

/**
 * @brief 销毁聚合器
 */
void cli_cfg_anchor_agg_free(cli_cfg_anchor_aggregator_t *agg);

#endif /* CLI_CFG_ANCHOR_H */
