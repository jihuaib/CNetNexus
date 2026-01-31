/**
 * @file   nn_cfg.h
 * @brief  CLI 配置模块公共接口，定义消息类型、视图、TLV 协议格式及解析工具
 * @author jhb
 * @date   2026/01/22
 */

#ifndef NN_CFG_H
#define NN_CFG_H

#include <arpa/inet.h>
#include <glib.h>
#include <stdint.h>
#include <string.h>

// ============================================================================
// CLI 消息类型定义
// ============================================================================

/** CLI 命令消息 */
#define NN_CFG_MSG_TYPE_CLI 0x00000001
/** CLI 响应消息 */
#define NN_CFG_MSG_TYPE_CLI_RESP 0x00000002
/** CLI 视图切换消息 */
#define NN_CFG_MSG_TYPE_CLI_VIEW_CHG 0x00000003
/** CLI 响应（还有更多数据待发送） */
#define NN_CFG_MSG_TYPE_CLI_RESP_MORE 0x00000004
/** CLI 请求下一批数据 */
#define NN_CFG_MSG_TYPE_CLI_CONTINUE 0x00000005

/** CLI 响应消息最大长度 */
#define NN_CFG_CLI_MAX_RESP_LEN 4096

// ============================================================================
// CLI 视图 ID 定义
// ============================================================================

/** 全局视图 */
#define NN_CFG_CLI_VIEW_GLOBAL 0x00000001
/** 用户视图 */
#define NN_CFG_CLI_VIEW_USER 0x00000002
/** 配置视图 */
#define NN_CFG_CLI_VIEW_CONFIG 0x00000003
/** BGP 视图 */
#define NN_CFG_CLI_VIEW_BGP 0x00000004
/** 接口视图 */
#define NN_CFG_CLI_VIEW_IF 0x00000005

/** 视图名称最大长度 */
#define NN_CFG_CLI_MAX_VIEW_NAME_LEN 20
/** 视图最大长度 */
#define NN_CFG_CLI_MAX_VIEW_LEN 64
/** 提示符最大长度 */
#define NN_CFG_CLI_MAX_PROMPT_LEN 128

// ============================================================================
// TLV 格式定义
// ============================================================================

// TLV 消息格式：
// [组 ID: 4 字节] [元素1: ID(4) + 长度(2) + 值] [元素2: ...] ...

/** TLV 组 ID 大小（字节） */
#define NN_CFG_TLV_GROUP_ID_SIZE 4
/** TLV 元素 ID 大小（字节） */
#define NN_CFG_TLV_ELEMENT_ID_SIZE 4
/** TLV 长度字段大小（字节） */
#define NN_CFG_TLV_LENGTH_SIZE 2
/** TLV 头部大小（元素 ID + 长度） */
#define NN_CFG_TLV_HEADER_SIZE (NN_CFG_TLV_ELEMENT_ID_SIZE + NN_CFG_TLV_LENGTH_SIZE)

/* 前向声明 */
typedef struct nn_cli_param_type nn_cli_param_type_t;

// ============================================================================
// TLV 解析器上下文
// ============================================================================

/**
 * @brief TLV 解析器上下文结构
 */
typedef struct nn_cfg_tlv_parser
{
    const uint8_t *data; /**< 原始数据缓冲区 */
    uint32_t total_len;  /**< 缓冲区总长度 */
    uint32_t offset;     /**< 当前解析偏移量 */
    uint32_t group_id;   /**< 已解析的组 ID */
} nn_cfg_tlv_parser_t;

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
static inline int nn_cfg_tlv_parser_init(nn_cfg_tlv_parser_t *parser, const uint8_t *data, uint32_t len)
{
    if (!parser || !data || len < NN_CFG_TLV_GROUP_ID_SIZE)
    {
        return -1;
    }

    parser->data = data;
    parser->total_len = len;
    parser->offset = 0;

    // 解析组 ID（前 4 字节，网络字节序）
    uint32_t group_id_be;
    memcpy(&group_id_be, data, NN_CFG_TLV_GROUP_ID_SIZE);
    parser->group_id = ntohl(group_id_be);
    parser->offset = NN_CFG_TLV_GROUP_ID_SIZE;

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
static inline int nn_cfg_tlv_parser_next(nn_cfg_tlv_parser_t *parser, uint32_t *out_id, const uint8_t **out_value,
                                         uint16_t *out_len)
{
    if (!parser || !out_id || !out_value || !out_len)
    {
        return -1;
    }

    // 检查是否有足够数据容纳 TLV 头部
    if (parser->offset + NN_CFG_TLV_HEADER_SIZE > parser->total_len)
    {
        return 0; // 无更多元素
    }

    // 解析元素 ID（4 字节，网络字节序）
    uint32_t elem_id_be;
    memcpy(&elem_id_be, parser->data + parser->offset, NN_CFG_TLV_ELEMENT_ID_SIZE);
    *out_id = ntohl(elem_id_be);
    parser->offset += NN_CFG_TLV_ELEMENT_ID_SIZE;

    // 解析长度（2 字节，网络字节序）
    uint16_t len_be;
    memcpy(&len_be, parser->data + parser->offset, NN_CFG_TLV_LENGTH_SIZE);
    *out_len = ntohs(len_be);
    parser->offset += NN_CFG_TLV_LENGTH_SIZE;

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
 * 用法：
 *   NN_CFG_TLV_PARSE_BEGIN(msg->data, msg->data_len, parser, group_id) {
 *       NN_CFG_TLV_FOREACH(parser, elem_id, value, len) {
 *           // 处理元素
 *       }
 *   } NN_CFG_TLV_PARSE_END()
 */
#define NN_CFG_TLV_PARSE_BEGIN(_data, _len, _parser_var, _group_id_var)                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        nn_cfg_tlv_parser_t _parser_var;                                                                               \
        if (nn_cfg_tlv_parser_init(&(_parser_var), (const uint8_t *)(_data), (_len)) == 0)                             \
        {                                                                                                              \
            uint32_t _group_id_var = (_parser_var).group_id;

/** TLV 解析结束宏 */
#define NN_CFG_TLV_PARSE_END()                                                                                         \
    }                                                                                                                  \
    }                                                                                                                  \
    while (0)

/**
 * @brief 遍历所有 TLV 元素
 */
#define NN_CFG_TLV_FOREACH(_parser_var, _id_var, _value_var, _len_var)                                                 \
    uint32_t _id_var;                                                                                                  \
    const uint8_t *_value_var;                                                                                         \
    uint16_t _len_var;                                                                                                 \
    int _tlv_ret_##parser_var;                                                                                         \
    while ((_tlv_ret_##parser_var = nn_cfg_tlv_parser_next(&(_parser_var), &(_id_var), &(_value_var), &_len_var)) == 1)

/**
 * @brief 从 TLV 元素中提取字符串值
 * @param _value_ptr 值指针
 * @param _len 值长度
 * @param _out_str 输出字符串缓冲区
 * @param _max_len 缓冲区最大长度
 */
#define NN_CFG_TLV_GET_STRING(_value_ptr, _len, _out_str, _max_len)                                                    \
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
 * @param _value_ptr 值指针
 * @param _len 值长度
 * @param _out_val 输出变量
 */
#define NN_CFG_TLV_GET_UINT32(_value_ptr, _len, _out_val)                                                              \
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
 * @param _value_ptr 值指针
 * @param _len 值长度
 * @param _out_val 输出变量
 */
#define NN_CFG_TLV_GET_UINT16(_value_ptr, _len, _out_val)                                                              \
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
 * @param _value_ptr 值指针
 * @param _len 值长度
 * @param _out_val 输出变量
 */
#define NN_CFG_TLV_GET_UINT8(_value_ptr, _len, _out_val)                                                               \
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
 * @param _value_ptr 值指针
 * @param _len 值长度
 * @param _out_str 输出字符串缓冲区
 * @param _max_len 缓冲区最大长度
 */
#define NN_CFG_TLV_GET_IPV4(_value_ptr, _len, _out_str, _max_len)                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((_value_ptr) && (_len) == 4)                                                                               \
        {                                                                                                              \
            snprintf((_out_str), (_max_len), "%u.%u.%u.%u", (_value_ptr)[0], (_value_ptr)[1], (_value_ptr)[2],         \
                     (_value_ptr)[3]);                                                                                 \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (out_str)[0] = '\0';                                                                                       \
        }                                                                                                              \
    } while (0)

// ============================================================================
// 公共 API
// ============================================================================

/**
 * @brief 注册模块的 XML 配置文件路径
 * @param module_id 模块 ID
 * @param xml_path XML 配置文件路径
 *
 * 模块应在其 constructor 中调用 nn_dev_register_module 之后调用此函数
 */
void nn_cfg_register_module_xml(uint32_t module_id, const char *xml_path);

/**
 * @brief 根据视图 ID 获取视图提示符模板
 * @param view_id 视图 ID
 * @param view_name 输出视图名称缓冲区
 * @return 成功返回 0，失败返回 -1
 */
int nn_cfg_get_view_prompt_template(uint32_t view_id, char *view_name);

/**
 * @brief 解析类型字符串为参数类型结构
 * @param type_str 类型字符串（如 "string(1-63)" 或 "uint(0-65535)"）
 * @return 新分配的参数类型结构，错误时返回 NULL
 */
nn_cli_param_type_t *nn_cfg_param_type_parse(const char *type_str);

/**
 * @brief 释放参数类型结构
 * @param param_type 待释放的参数类型结构
 */
void nn_cfg_param_type_free(nn_cli_param_type_t *param_type);

/**
 * @brief 根据参数类型定义验证值的有效性
 * @param param_type 参数类型定义
 * @param value 待验证的值
 * @param error_msg 错误信息输出缓冲区
 * @param error_msg_size 错误信息缓冲区大小
 * @return 有效返回 TRUE，无效返回 FALSE
 */
gboolean nn_cfg_param_type_validate(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                                    uint32_t error_msg_size);

#endif // NN_CFG_H
