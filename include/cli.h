/**
 * @file   cli.h
 * @brief  CLI 配置模块公共接口，定义消息类型、视图、TLV 协议格式及解析工具
 * @author jhb
 * @date   2026/01/22
 */

#ifndef CLI_H
#define CLI_H

#include <arpa/inet.h>
#include <glib.h>
#include <stdint.h>
#include <string.h>

#include "ipc.h"

// ============================================================================
// CLI 消息类型定义（使用 IPC 编码）
// ============================================================================

/** CLI 命令消息 */
#define CFG_MSG_TYPE_CLI IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0001)
/** CLI 响应消息 */
#define CFG_MSG_TYPE_CLI_RESP IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0002)
/** CLI 视图切换消息 */
#define CFG_MSG_TYPE_CLI_VIEW_CHG IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0003)
/** CLI 响应（还有更多数据待发送） */
#define CFG_MSG_TYPE_CLI_RESP_MORE IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0004)
/** CLI 请求下一批数据 */
#define CFG_MSG_TYPE_CLI_CONTINUE IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0005)

/** CLI 响应消息最大长度 */
#define CLI_MAX_RESP_LEN 4096

// ============================================================================
// CLI 视图 ID 定义
// ============================================================================

/** 全局视图 */
#define CLI_VIEW_GLOBAL 0x00000001
/** 用户视图 */
#define CLI_VIEW_USER 0x00000002
/** 配置视图 */
#define CLI_VIEW_CONFIG 0x00000003
/** BGP 视图 */
#define CLI_VIEW_BGP 0x00000004
/** 接口视图 */
#define CLI_VIEW_IF 0x00000005

/** 视图名称最大长度 */
#define CLI_CLI_MAX_VIEW_LEN 20
/** 视图最大长度 */
#define CFG_CLI_MAX_VIEW_LEN 64
/** 提示符最大长度 */
#define CLI_CLI_MAX_PROMPT_LEN 128

// ============================================================================
// TLV 格式定义
// ============================================================================

// TLV 消息格式：
// [组 ID: 4 字节] [元素1: ID(4) + 长度(2) + 值] [元素2: ...] ...

/** TLV 组 ID 大小（字节） */
#define CFG_TLV_GROUP_ID_SIZE 4
/** TLV 元素 ID 大小（字节） */
#define CFG_TLV_ELEMENT_ID_SIZE 4
/** TLV 长度字段大小（字节） */
#define CFG_TLV_LENGTH_SIZE 2
/** TLV 头部大小（元素 ID + 长度） */
#define CFG_TLV_HEADER_SIZE (CFG_TLV_ELEMENT_ID_SIZE + CFG_TLV_LENGTH_SIZE)

/** 上下文 TLV cfg_id 标记位，用于区分上下文变量和命令参数 */
#define CFG_TLV_CONTEXT_FLAG 0x80000000

/** 判断 cfg_id 是否为上下文变量 */
#define CFG_TLV_IS_CONTEXT(cfg_id) (((cfg_id) & CFG_TLV_CONTEXT_FLAG) != 0)

/** 提取上下文变量的原始 cfg_id */
#define CFG_TLV_CONTEXT_ID(cfg_id) ((cfg_id) & ~CFG_TLV_CONTEXT_FLAG)

/* 前向声明 */
typedef struct cli_param_type cli_param_type_t;

// ============================================================================
// 公共 API
// ============================================================================

/**
 * @brief 注册模块的 XML 配置文件路径
 * @param module_id 模块 ID
 * @param xml_path XML 配置文件路径
 *
 * 模块应在其 constructor 中调用 dev_register_module 之后调用此函数
 */
void cfg_register_module_xml(uint32_t module_id, const char *xml_path);

/**
 * @brief 根据视图 ID 获取视图提示符模板
 * @param view_id 视图 ID
 * @param view_name 输出视图名称缓冲区
 * @return 成功返回 0，失败返回 -1
 */
int cfg_get_view_prompt_template(uint32_t view_id, char *view_name);

/**
 * @brief 解析类型字符串为参数类型结构
 * @param type_str 类型字符串（如 "string(1-63)" 或 "uint(0-65535)"）
 * @return 新分配的参数类型结构，错误时返回 NULL
 */
cli_param_type_t *cfg_param_type_parse(const char *type_str);

/**
 * @brief 释放参数类型结构
 * @param param_type 待释放的参数类型结构
 */
void cfg_param_type_free(cli_param_type_t *param_type);

/**
 * @brief 根据参数类型定义验证值的有效性
 * @param param_type 参数类型定义
 * @param value 待验证的值
 * @param error_msg 错误信息输出缓冲区
 * @param error_msg_size 错误信息缓冲区大小
 * @return 有效返回 TRUE，无效返回 FALSE
 */
gboolean cfg_param_type_validate(const cli_param_type_t *param_type, const char *value, char *error_msg,
                                 uint32_t error_msg_size);

/**
 * @brief 获取配置模板（由 XML 解析器加载）
 * @param template_name 模板名称
 * @return 模板指针（不转移所有权），未找到返回 NULL
 */
struct config_template *cfg_get_config_template(const char *template_name);

/**
 * @brief 根据模板和变量映射生成格式化的配置输出
 * @param template_name 模板名称
 * @param var_values 变量替换映射（GHashTable：key=variable_name, value=variable_value_string）
 * @return 渲染后的字符串（调用者负责 g_free），失败返回 NULL
 */
char *cfg_render_template(const char *template_name, GHashTable *var_values);

// ============================================================================
// DB table 载荷格式定义
// ============================================================================

/** 载荷 flags: "no" 前缀命令（删除操作） */
#define CLI_PAYLOAD_FLAG_NO_CMD 0x01

/** 字段 flags: 上下文字段（非当前命令参数） */
#define CLI_FIELD_FLAG_CONTEXT 0x01

/** 判断是否为 no 命令 */
#define CLI_DB_IS_NO_CMD(p) (((p)->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0)

/** 判断字段是否为上下文字段 */
#define CLI_DB_IS_CONTEXT(ff) (((ff) & CLI_FIELD_FLAG_CONTEXT) != 0)

/**
 * @brief DB table 载荷解析器
 */
typedef struct cli_db_payload_parser
{
    uint8_t flags;        /**< 载荷标志位 */
    char *db_name;        /**< 数据库名称（已分配，cleanup 释放） */
    char *table_name;     /**< 表名称（已分配，cleanup 释放） */
    uint16_t num_fields;  /**< 总字段数 */
    uint16_t fields_read; /**< 已读取字段数 */

    /* 内部读取状态 */
    const uint8_t *_reader_data; /**< 原始数据缓冲区 */
    uint32_t _reader_len;        /**< 缓冲区总长度 */
    uint32_t _reader_pos;        /**< 当前读取位置 */
} cli_db_payload_parser_t;

/**
 * @brief 初始化 DB table 载荷解析器
 * @param p 解析器
 * @param data 载荷数据
 * @param len 数据长度
 * @return 成功返回 0，失败返回 -1
 */
int cli_db_payload_init(cli_db_payload_parser_t *p, const uint8_t *data, uint32_t len);

/**
 * @brief 读取下一个字段
 * @param p 解析器
 * @param out_flags 输出字段标志位
 * @param out_field_name 输出字段名（调用者 g_free）
 * @param out_value 输出字段值（调用者 db_value_free）
 * @return 1=成功读取, 0=无更多字段, -1=错误
 */
int cli_db_payload_next(cli_db_payload_parser_t *p, uint8_t *out_flags, char **out_field_name, void *out_value);

/**
 * @brief 清理载荷解析器资源
 * @param p 解析器
 */
void cli_db_payload_cleanup(cli_db_payload_parser_t *p);

/* 向后兼容别名（cfg_ → cli_） */
#define cfg_db_payload_init cli_db_payload_init
#define cfg_db_payload_next cli_db_payload_next
#define cfg_db_payload_cleanup cli_db_payload_cleanup
#define CFG_DB_IS_NO_CMD CLI_DB_IS_NO_CMD

// ============================================================================
// 公共 API
// ============================================================================

/**
 * @brief 注册模块的 XML 配置文件路径
 * @param module_id 模块 ID
 * @param xml_path XML 配置文件路径
 */
void cli_register_module_xml(uint32_t module_id, const char *xml_path);

/**
 * @brief 根据视图 ID 获取视图提示符模板
 * @param view_id 视图 ID
 * @param view_name 输出视图名称缓冲区
 * @return 成功返回 0，失败返回 -1
 */
int cli_get_view_prompt_template(uint32_t view_id, char *view_name);

/**
 * @brief 解析类型字符串为参数类型结构
 * @param type_str 类型字符串（如 "string(1-63)" 或 "uint(0-65535)"）
 * @return 新分配的参数类型结构，错误时返回 NULL
 */
cli_param_type_t *cli_param_type_parse_str(const char *type_str);

/**
 * @brief 释放参数类型结构
 * @param param_type 待释放的参数类型结构
 */
void cli_param_type_free_str(cli_param_type_t *param_type);

/**
 * @brief 根据参数类型定义验证值的有效性
 * @param param_type 参数类型定义
 * @param value 待验证的值
 * @param error_msg 错误信息输出缓冲区
 * @param error_msg_size 错误信息缓冲区大小
 * @return 有效返回 TRUE，无效返回 FALSE
 */
gboolean cli_param_type_validate_str(const cli_param_type_t *param_type, const char *value, char *error_msg,
                                     uint32_t error_msg_size);

/**
 * @brief 获取配置模板（由 XML 解析器加载）
 * @param template_name 模板名称
 * @return 模板指针（不转移所有权），未找到返回 NULL
 */
struct config_template *cli_get_config_template(const char *template_name);

/**
 * @brief 根据模板和变量映射生成格式化的配置输出
 * @param template_name 模板名称
 * @param var_values 变量替换映射
 * @return 渲染后的字符串（调用者负责 g_free），失败返回 NULL
 */
char *cli_render_template(const char *template_name, GHashTable *var_values);

// ============================================================================
// CLI Engine 和 XML Parser 类型定义
// ============================================================================

// ============================================================================
// CLI Engine 公共 API
// ============================================================================

/**
 * @brief 初始化 CLI 引擎
 * @param module_id 当前模块 ID（用于 IPC）
 * @param module_name 模块名称
 * @return CLI 引擎上下文，失败返回 NULL
 */
struct cli_engine_context *cli_engine_init(uint32_t module_id, const char *module_name);

/**
 * @brief 清理 CLI 引擎资源
 * @param ctx CLI 引擎上下文
 */
void cli_engine_cleanup(struct cli_engine_context *ctx);

/**
 * @brief 获取视图树（用于加载 XML CLI 定义）
 * @param ctx CLI 引擎上下文
 * @return 视图树指针
 */
struct cli_view_tree *cli_engine_get_view_tree(struct cli_engine_context *ctx);

/**
 * @brief 获取 IPC 上下文
 * @param ctx CLI 引擎上下文
 * @return IPC 上下文指针
 */
ipc_context_t *cli_engine_get_ipc_context(struct cli_engine_context *ctx);

/**
 * @brief 设置用户数据
 * @param ctx CLI 引擎上下文
 * @param user_data 用户数据指针
 */
void cli_engine_set_user_data(struct cli_engine_context *ctx, void *user_data);

/**
 * @brief 获取用户数据
 * @param ctx CLI 引擎上下文
 * @return 用户数据指针
 */
void *cli_engine_get_user_data(struct cli_engine_context *ctx);

/**
 * @brief 创建 CLI 会话（由接入模块调用）
 * @param ctx CLI 引擎上下文
 * @param client_fd 客户端文件描述符
 * @return CLI 会话指针，失败返回 NULL
 */
struct cli_session *cli_engine_create_session(struct cli_engine_context *ctx, int client_fd);

/**
 * @brief 销毁 CLI 会话
 * @param session CLI 会话指针
 */
void cli_engine_destroy_session(struct cli_session *session);

/**
 * @brief 处理客户端输入（由接入模块在收到数据时调用）
 * @param session CLI 会话指针
 * @return 0 成功，-1 表示会话应关闭
 */
int cli_engine_process_input(struct cli_session *session);

// ============================================================================
// CLI XML Parser 公共 API
// ============================================================================

#endif // CLI_H
