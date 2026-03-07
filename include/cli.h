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
/** VRF 配置视图 */
#define CLI_VIEW_VRF 0x00000008

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

/**
 * 上下文 TLV 类型字节（type 字段）。
 * 值 0x80 不与 DB_TYPE_* (0-4) 冲突。
 * Context 条目格式: [ctx_id:u32][CLI_TLV_TYPE_CTX:u8][len:u16][value...]
 * 命令参数条目格式: [cfg_id:u32][DB_TYPE_*:u8][len:u16][value...]
 */
#define CLI_TLV_TYPE_CTX 0x80

/** 判断 TLV 条目是否为上下文变量（基于 type 字节，不再使用 cfg_id 标志位） */
#define CLI_TLV_IS_CTX(entry_ptr) ((entry_ptr)->type == CLI_TLV_TYPE_CTX)

/** 视图模板 TLV 专用 cfg_id（CFG 在发送视图切换命令时附带，模块提取后填充动态参数） */
#define CFG_TLV_VIEW_TEMPLATE_ID 0x40000000

/** 判断 cfg_id 是否为视图模板条目 */
#define CFG_TLV_IS_VIEW_TEMPLATE(cfg_id) ((cfg_id) == CFG_TLV_VIEW_TEMPLATE_ID)

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
