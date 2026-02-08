/**
 * @file   cli.h
 * @brief  CLI 模块公共接口，定义消息类型、视图、TLV 协议格式及解析工具
 * @author jhb
 * @date   2026/02/08
 */

#ifndef CLI_H
#define CLI_H

#include <arpa/inet.h>
#include <glib.h>
#include <stdint.h>
#include <string.h>

#include "ipc.h"

// ============================================================================
// CLI 消息子类（大类 = IPC_CATEGORY_CLI）
// ============================================================================

/** CLI 命令消息 */
#define CLI_MSG_TYPE_CLI IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0001)
/** CLI 响应消息 */
#define CLI_MSG_TYPE_CLI_RESP IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0002)
/** CLI 视图切换消息 */
#define CLI_MSG_TYPE_CLI_VIEW_CHG IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0003)
/** CLI 响应（还有更多数据待发送） */
#define CLI_MSG_TYPE_CLI_RESP_MORE IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0004)
/** CLI 请求下一批数据 */
#define CLI_MSG_TYPE_CLI_CONTINUE IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0005)
/** CLI 获取视图提示符请求 */
#define CLI_MSG_TYPE_GET_VIEW_PROMPT IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0006)

/** CLI 响应消息最大长度 */
#define CLI_CLI_MAX_RESP_LEN 4096

// ============================================================================
// CLI 视图 ID 定义
// ============================================================================

/** 全局视图 */
#define CLI_CLI_VIEW_GLOBAL 0x00000001
/** 用户视图 */
#define CLI_CLI_VIEW_USER 0x00000002
/** 配置视图 */
#define CLI_CLI_VIEW_CONFIG 0x00000003
/** BGP 视图 */
#define CLI_CLI_VIEW_BGP 0x00000004
/** 接口视图 */
#define CLI_CLI_VIEW_IF 0x00000005

/** 视图名称最大长度 */
#define CLI_CLI_MAX_VIEW_NAME_LEN 20
/** 视图最大长度 */
#define CLI_CLI_MAX_VIEW_LEN 64
/** 提示符最大长度 */
#define CLI_CLI_MAX_PROMPT_LEN 128

// ============================================================================
// TLV 格式定义
// ============================================================================

// TLV 消息格式：
// [组 ID: 4 字节] [元素1: ID(4) + 长度(2) + 值] [元素2: ...] ...

/** TLV 组 ID 大小（字节） */
#define CLI_TLV_GROUP_ID_SIZE 4
/** TLV 元素 ID 大小（字节） */
#define CLI_TLV_ELEMENT_ID_SIZE 4
/** TLV 长度字段大小（字节） */
#define CLI_TLV_LENGTH_SIZE 2
/** TLV 头部大小（元素 ID + 长度） */
#define CLI_TLV_HEADER_SIZE (CLI_TLV_ELEMENT_ID_SIZE + CLI_TLV_LENGTH_SIZE)

/** 上下文 TLV cfg_id 标记位，用于区分上下文变量和命令参数 */
#define CLI_TLV_CONTEXT_FLAG 0x80000000

/** 判断 cfg_id 是否为上下文变量 */
#define CLI_TLV_IS_CONTEXT(cfg_id) (((cfg_id) & CLI_TLV_CONTEXT_FLAG) != 0)

/** 提取上下文变量的原始 cfg_id */
#define CLI_TLV_CONTEXT_ID(cfg_id) ((cfg_id) & ~CLI_TLV_CONTEXT_FLAG)

/* 前向声明 */
typedef struct cli_param_type cli_param_type_t;

// ============================================================================
// TLV 解析器上下文
// ============================================================================

/**
 * @brief TLV 解析器上下文结构
 */
typedef struct cli_tlv_parser
{
    const uint8_t *data; /**< 原始数据缓冲区 */
    uint32_t total_len;  /**< 缓冲区总长度 */
    uint32_t offset;     /**< 当前解析偏移量 */
    uint32_t group_id;   /**< 已解析的组 ID */
} cli_tlv_parser_t;

// ============================================================================
// TLV 解析器函数
// ============================================================================

/**
 * @brief 初始化 TLV 解析器
 * @param parser 解析器上下文
 * @param data TLV 数据缓冲区
 * @param len 缓冲区长度
 * @return 成功返回 0，失败返回 -1
 */
static inline int cli_tlv_parser_init(cli_tlv_parser_t *parser, const uint8_t *data, uint32_t len)
{
    if (!parser || !data || len < CLI_TLV_GROUP_ID_SIZE)
    {
        return -1;
    }

    parser->data = data;
    parser->total_len = len;
    parser->offset = 0;

    // 解析组 ID（前 4 字节，网络字节序）
    uint32_t group_id_be;
    memcpy(&group_id_be, data, CLI_TLV_GROUP_ID_SIZE);
    parser->group_id = ntohl(group_id_be);
    parser->offset = CLI_TLV_GROUP_ID_SIZE;

    return 0;
}

/**
 * @brief 获取下一个 TLV 元素
 * @param parser 解析器上下文
 * @param out_id 输出元素 ID
 * @param out_value 输出值指针（指向原始缓冲区）
 * @param out_len 输出值长度
 * @return 找到元素返回 1，无更多元素返回 0，错误返回 -1
 */
static inline int cli_tlv_parser_next(cli_tlv_parser_t *parser, uint32_t *out_id, const uint8_t **out_value,
                                         uint16_t *out_len)
{
    if (!parser || !out_id || !out_value || !out_len)
    {
        return -1;
    }

    // 检查是否有足够数据容纳 TLV 头部
    if (parser->offset + CLI_TLV_HEADER_SIZE > parser->total_len)
    {
        return 0; // 无更多元素
    }

    // 解析元素 ID（4 字节，网络字节序）
    uint32_t elem_id_be;
    memcpy(&elem_id_be, parser->data + parser->offset, CLI_TLV_ELEMENT_ID_SIZE);
    *out_id = ntohl(elem_id_be);
    parser->offset += CLI_TLV_ELEMENT_ID_SIZE;

    // 解析长度（2 字节，网络字节序）
    uint16_t len_be;
    memcpy(&len_be, parser->data + parser->offset, CLI_TLV_LENGTH_SIZE);
    *out_len = ntohs(len_be);
    parser->offset += CLI_TLV_LENGTH_SIZE;

    // 检查是否有足够数据容纳值
    if (parser->offset + *out_len > parser->total_len)
    {
        return -1; // TLV 格式错误
    }

    // 设置值指针（指向原始缓冲区）
    *out_value = (*out_len > 0) ? (parser->data + parser->offset) : NULL;
    parser->offset += *out_len;

    return 1;
}

// ============================================================================
// TLV 解析便捷宏
// ============================================================================

/**
 * @brief 解析 TLV 消息并遍历元素
 */
#define CLI_TLV_PARSE_BEGIN(_data, _len, _parser_var, _group_id_var)                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        cli_tlv_parser_t _parser_var;                                                                               \
        if (cli_tlv_parser_init(&(_parser_var), (const uint8_t *)(_data), (_len)) == 0)                             \
        {                                                                                                              \
            uint32_t _group_id_var = (_parser_var).group_id;

/** TLV 解析结束宏 */
#define CLI_TLV_PARSE_END()                                                                                         \
    }                                                                                                                  \
    }                                                                                                                  \
    while (0)

/**
 * @brief 遍历所有 TLV 元素
 */
#define CLI_TLV_FOREACH(_parser_var, _id_var, _value_var, _len_var)                                                 \
    uint32_t _id_var;                                                                                                  \
    const uint8_t *_value_var;                                                                                         \
    uint16_t _len_var;                                                                                                 \
    int _tlv_ret_##parser_var;                                                                                         \
    while ((_tlv_ret_##parser_var = cli_tlv_parser_next(&(_parser_var), &(_id_var), &(_value_var), &_len_var)) == 1)

/**
 * @brief 从 TLV 元素中提取字符串值
 */
#define CLI_TLV_GET_STRING(_value_ptr, _len, _out_str, _max_len)                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        uint32_t _copy_len = ((_len) < (_max_len) - 1) ? (_len) : ((_max_len) - 1);                                    \
        if ((_value_ptr) && (_copy_len) > 0)                                                                           \
        {                                                                                                              \
            memcpy((_out_str), (_value_ptr), (_copy_len));                                                             \
            (_out_str)[(_copy_len)] = '\0';                                                                            \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (_out_str)[0] = '\0';                                                                                      \
        }                                                                                                              \
    } while (0)

/**
 * @brief 从 TLV 元素中提取 uint32_t 值（网络字节序）
 */
#define CLI_TLV_GET_UINT32(_value_ptr, _len, _out_val)                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((_value_ptr) && (_len) == sizeof(uint32_t))                                                                \
        {                                                                                                              \
            uint32_t _val_be;                                                                                          \
            memcpy(&(_val_be), (_value_ptr), sizeof(uint32_t));                                                        \
            (_out_val) = ntohl(_val_be);                                                                               \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (_out_val) = 0;                                                                                            \
        }                                                                                                              \
    } while (0)

/**
 * @brief 从 TLV 元素中提取 uint16_t 值（网络字节序）
 */
#define CLI_TLV_GET_UINT16(_value_ptr, _len, _out_val)                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((_value_ptr) && (_len) == sizeof(uint16_t))                                                                \
        {                                                                                                              \
            uint16_t _val_be;                                                                                          \
            memcpy(&(_val_be), (_value_ptr), sizeof(uint16_t));                                                        \
            (_out_val) = ntohs(_val_be);                                                                               \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (_out_val) = 0;                                                                                            \
        }                                                                                                              \
    } while (0)

/**
 * @brief 从 TLV 元素中提取 uint8_t 值
 */
#define CLI_TLV_GET_UINT8(_value_ptr, _len, _out_val)                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((_value_ptr) && (_len) == sizeof(uint8_t))                                                                 \
        {                                                                                                              \
            (_out_val) = *(_value_ptr);                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (_out_val) = 0;                                                                                            \
        }                                                                                                              \
    } while (0)

/**
 * @brief 从 TLV 元素中提取 IPv4 地址
 */
#define CLI_TLV_GET_IPV4(_value_ptr, _len, _out_str, _max_len)                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((_value_ptr) && (_len) == 4)                                                                               \
        {                                                                                                              \
            snprintf((_out_str), (_max_len), "%u.%u.%u.%u", (_value_ptr)[0], (_value_ptr)[1], (_value_ptr)[2],         \
                     (_value_ptr)[3]);                                                                                 \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (_out_str)[0] = '\0';                                                                                      \
        }                                                                                                              \
    } while (0)

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
    uint8_t flags;      /**< 载荷标志位 */
    char *db_name;      /**< 数据库名称（已分配，cleanup 释放） */
    char *table_name;   /**< 表名称（已分配，cleanup 释放） */
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
int cli_db_payload_next(cli_db_payload_parser_t *p, uint8_t *out_flags, char **out_field_name,
                           void *out_value);

/**
 * @brief 清理载荷解析器资源
 * @param p 解析器
 */
void cli_db_payload_cleanup(cli_db_payload_parser_t *p);

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

/* 类型别名(与内部头文件保持兼容) */
typedef struct cli_session cli_session_t;
typedef struct cli_view_tree cli_view_tree_t;
typedef struct cli_engine_context cli_engine_context_t;

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

/**
 * @brief 从 XML 文件加载 CLI 视图树
 * @param xml_file XML 文件路径
 * @param view_tree 视图树指针
 * @return 成功返回 0，失败返回非 0
 */
uint32_t cli_xml_load_view_tree(const char *xml_file, struct cli_view_tree *view_tree);

// ============================================================================
// 向后兼容别名（过渡期使用）
// ============================================================================

/** @deprecated 使用 CLI_ 前缀的新名称 */
#define CFG_MSG_TYPE_CLI CLI_MSG_TYPE_CLI
#define CFG_MSG_TYPE_CLI_RESP CLI_MSG_TYPE_CLI_RESP
#define CFG_MSG_TYPE_CLI_VIEW_CHG CLI_MSG_TYPE_CLI_VIEW_CHG
#define CFG_MSG_TYPE_CLI_RESP_MORE CLI_MSG_TYPE_CLI_RESP_MORE
#define CFG_MSG_TYPE_CLI_CONTINUE CLI_MSG_TYPE_CLI_CONTINUE
#define CFG_MSG_TYPE_GET_VIEW_PROMPT CLI_MSG_TYPE_GET_VIEW_PROMPT

#define CFG_CLI_MAX_RESP_LEN CLI_CLI_MAX_RESP_LEN

#define CFG_CLI_VIEW_GLOBAL CLI_CLI_VIEW_GLOBAL
#define CFG_CLI_VIEW_USER CLI_CLI_VIEW_USER
#define CFG_CLI_VIEW_CONFIG CLI_CLI_VIEW_CONFIG
#define CFG_CLI_VIEW_BGP CLI_CLI_VIEW_BGP
#define CFG_CLI_VIEW_IF CLI_CLI_VIEW_IF

#define CFG_CLI_MAX_VIEW_NAME_LEN CLI_CLI_MAX_VIEW_NAME_LEN
#define CFG_CLI_MAX_VIEW_LEN CLI_CLI_MAX_VIEW_LEN
#define CFG_CLI_MAX_PROMPT_LEN CLI_CLI_MAX_PROMPT_LEN

#define CFG_TLV_GROUP_ID_SIZE CLI_TLV_GROUP_ID_SIZE
#define CFG_TLV_ELEMENT_ID_SIZE CLI_TLV_ELEMENT_ID_SIZE
#define CFG_TLV_LENGTH_SIZE CLI_TLV_LENGTH_SIZE
#define CFG_TLV_HEADER_SIZE CLI_TLV_HEADER_SIZE
#define CFG_TLV_CONTEXT_FLAG CLI_TLV_CONTEXT_FLAG
#define CFG_TLV_IS_CONTEXT CLI_TLV_IS_CONTEXT
#define CFG_TLV_CONTEXT_ID CLI_TLV_CONTEXT_ID

#define CFG_PAYLOAD_FLAG_NO_CMD CLI_PAYLOAD_FLAG_NO_CMD
#define CFG_FIELD_FLAG_CONTEXT CLI_FIELD_FLAG_CONTEXT
#define CFG_DB_IS_NO_CMD CLI_DB_IS_NO_CMD
#define CFG_DB_IS_CONTEXT CLI_DB_IS_CONTEXT

/* 类型别名 */
typedef cli_tlv_parser_t cfg_tlv_parser_t;
typedef cli_db_payload_parser_t cfg_db_payload_parser_t;

/* TLV 解析器函数别名 */
#define cfg_tlv_parser_init cli_tlv_parser_init
#define cfg_tlv_parser_next cli_tlv_parser_next

/* TLV 宏别名 */
#define CFG_TLV_PARSE_BEGIN CLI_TLV_PARSE_BEGIN
#define CFG_TLV_PARSE_END CLI_TLV_PARSE_END
#define CFG_TLV_FOREACH CLI_TLV_FOREACH
#define CFG_TLV_GET_STRING CLI_TLV_GET_STRING
#define CFG_TLV_GET_UINT32 CLI_TLV_GET_UINT32
#define CFG_TLV_GET_UINT16 CLI_TLV_GET_UINT16
#define CFG_TLV_GET_UINT8 CLI_TLV_GET_UINT8
#define CFG_TLV_GET_IPV4 CLI_TLV_GET_IPV4

/* 载荷函数别名 */
#define cfg_db_payload_init cli_db_payload_init
#define cfg_db_payload_next cli_db_payload_next
#define cfg_db_payload_cleanup cli_db_payload_cleanup

/* API 函数别名 */
#define cfg_register_module_xml cli_register_module_xml
#define cfg_get_view_prompt_template cli_get_view_prompt_template
#define cfg_param_type_parse cli_param_type_parse_str
#define cfg_param_type_free cli_param_type_free_str
#define cfg_param_type_validate cli_param_type_validate_str
#define cfg_get_config_template cli_get_config_template
#define cfg_render_template cli_render_template

#endif // CLI_H
