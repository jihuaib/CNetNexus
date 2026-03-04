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

#include "dev.h"

// ============================================================================
// CLI 消息类型定义（使用 IPC 编码）
// ============================================================================

/** CLI 命令消息 */
#define CFG_MSG_TYPE_CLI DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0001)
/** CLI 响应消息 */
#define CFG_MSG_TYPE_CLI_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0002)
/** CLI 视图切换消息 */
#define CFG_MSG_TYPE_CLI_VIEW_CHG DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0003)
/** CLI 响应（还有更多数据待发送） */
#define CFG_MSG_TYPE_CLI_RESP_MORE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0004)
/** CLI 请求下一批数据 */
#define CFG_MSG_TYPE_CLI_CONTINUE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0005)
/** 向业务模块请求当前配置（用于 show current-configuration 汇聚） */
#define CFG_MSG_TYPE_SHOW_CONFIG DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0006)

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
/** 路由视图 */
#define CLI_VIEW_ROUTE 0x00000006
/** BGP 地址族 IPv4 单播视图 */
#define CLI_VIEW_BGP_AF_IPV4 0x00000007

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

// ============================================================================
// TLV 载荷格式定义
// ============================================================================

/** 载荷 flags: "no" 前缀命令（删除操作） */
#define CLI_PAYLOAD_FLAG_NO_CMD 0x01

/**
 * @brief TLV 条目（解析后的单个字段）
 */
typedef struct cli_tlv_entry
{
    uint32_t cfg_id; /**< 配置 ID（高位可能有 CONTEXT_FLAG） */
    uint8_t type;    /**< 值类型（DB_TYPE_*） */
    uint16_t length; /**< 值长度 */
    uint8_t *value;  /**< 值数据（已分配，entry_free 释放） */
} cli_tlv_entry_t;

/**
 * @brief TLV 载荷解析器
 */
typedef struct cli_tlv_parser
{
    uint8_t flags;     /**< 载荷标志位 */
    uint32_t group_id; /**< 命令组 ID */

    /* 内部读取状态 */
    const uint8_t *_data; /**< 原始数据缓冲区 */
    uint32_t _len;        /**< 缓冲区总长度 */
    uint32_t _pos;        /**< 当前读取位置 */
} cli_tlv_parser_t;

/**
 * @brief 初始化 TLV 载荷解析器
 * @param p 解析器
 * @param data 载荷数据
 * @param len 数据长度
 * @return 成功返回 0，失败返回 -1
 */
int cli_tlv_init(cli_tlv_parser_t *p, const uint8_t *data, uint32_t len);

/**
 * @brief 读取下一个 TLV 条目
 * @param p 解析器
 * @param entry 输出条目（调用者需 cli_tlv_entry_free）
 * @return 1=成功读取, 0=无更多条目, -1=错误
 */
int cli_tlv_next(cli_tlv_parser_t *p, cli_tlv_entry_t *entry);

/**
 * @brief 释放 TLV 条目资源
 * @param entry 条目
 */
void cli_tlv_entry_free(cli_tlv_entry_t *entry);

/**
 * @brief 清理 TLV 解析器资源
 * @param p 解析器
 */
void cli_tlv_cleanup(cli_tlv_parser_t *p);

/**
 * @brief 向 CLI 响应缓冲区追加格式化字符串
 * @param buf      目标字符缓冲区（char[]，非指针）
 * @param buf_size 缓冲区总大小（字节）
 * @param off      当前已写入偏移量（size_t 变量，宏内自动更新）
 * @param fmt      格式化字符串（及可变参数）
 *
 * 使用示例：
 * @code
 *   char buf[CLI_MAX_RESP_LEN];
 *   size_t off = 0;
 *   CLI_BUF_APPEND(buf, sizeof(buf), off, "value: %d\r\n", val);
 * @endcode
 */
#define CLI_BUF_APPEND(buf, buf_size, off, fmt, ...)                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((off) < (buf_size))                                                                                        \
        {                                                                                                              \
            int _cli_written = snprintf((buf) + (off), (buf_size) - (off), fmt, ##__VA_ARGS__);                        \
            if (_cli_written > 0)                                                                                      \
            {                                                                                                          \
                (off) += (size_t)_cli_written;                                                                         \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

/**
 * @brief 从 TLV 条目中读取整数值
 * @param entry TLV 条目
 * @return 整数值，类型不匹配返回 0
 */
int64_t cli_tlv_entry_get_int(const cli_tlv_entry_t *entry);

/**
 * @brief 从 TLV 条目中读取字符串值
 * @param entry TLV 条目
 * @return 字符串（内部指针，不转移所有权），类型不匹配返回 NULL
 */
const char *cli_tlv_entry_get_text(const cli_tlv_entry_t *entry);

#endif // CLI_H
