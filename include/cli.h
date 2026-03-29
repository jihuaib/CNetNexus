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
#define CLI_MSG_TYPE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0001)
/** CLI 响应消息 */
#define CLI_MSG_TYPE_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0002)
/** CLI 响应（还有更多数据待发送） */
#define CLI_MSG_TYPE_RESP_MORE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0004)
/** CLI 请求下一批数据 */
#define CLI_MSG_TYPE_CONTINUE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0005)
/** 向业务模块请求当前配置（用于 show current-configuration 汇聚） */
#define CLI_MSG_TYPE_SHOW_CONFIG DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0006)
/** CFG → 模块：查询动态补全候选值（payload: uint32_t cfg_id，网络字节序） */
#define CLI_MSG_TYPE_QUERY_CANDIDATES DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0007)
/** 模块 → CFG：返回候选值列表（payload: "val1\0val2\0\0"，双 null 结尾） */
#define CLI_MSG_TYPE_QUERY_CANDIDATES_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0008)

/** CLI 响应消息最大长度 */
#define CLI_MAX_RESP_LEN 4096

// ============================================================================
// CLI 视图名称定义（内存和 XML 均以名称字符串作为唯一标识）
// ============================================================================

/** 全局视图（命令在所有视图中可用） */
#define CLI_VIEW_GLOBAL "global"
/** 用户视图 */
#define CLI_VIEW_USER "user"
/** 配置视图 */
#define CLI_VIEW_CONFIG "config"
/** BGP 视图 */
#define CLI_VIEW_BGP "bgp"
/** 接口视图 */
#define CLI_VIEW_IF "if"
/** loop 接口视图 */
#define CLI_VIEW_IF_LOOP "if-loop"
/** 路由视图 */
#define CLI_VIEW_ROUTE "route"
/** BGP 地址族 IPv4 单播视图 */
#define CLI_VIEW_BGP_AF_IPV4 "bgp-af-ipv4-uni"
/** BGP 地址族 IPv6 单播视图 */
#define CLI_VIEW_BGP_AF_IPV6 "bgp-af-ipv6-uni"
/** VRF 配置视图 */
#define CLI_VIEW_VRF "vrf"

// ============================================================================
// CLI 上下文变量 ID 定义（全局唯一，新增时在此处登记，避免冲突）
// 上下文条目在会话中跨视图累积，ctx_id 必须全局不重复
// ============================================================================

/** BGP VRF ID 上下文 */
#define CLI_CTX_ID_BGP_VRF 1
/** BGP 地址族 AFI 上下文 */
#define CLI_CTX_ID_BGP_AFI 2
/** BGP 地址族 SAFI 上下文 */
#define CLI_CTX_ID_BGP_SAFI 3
/** 接口索引 ctx 变量：值 1-4 分别对应 GE-1 到 GE-4 */
#define CLI_CTX_ID_IF_IDX 4
/** VRF 名称上下文 */
#define CLI_CTX_ID_VRF_NAME 5
/** loop 接口编号 ctx 变量：值 1-2024 对应 loop1-loop2024 */
#define CLI_CTX_ID_IF_LOOP_IDX 6
/** BMP 实例名称上下文（字符串） */
#define CLI_CTX_ID_BMP_INST_NAME 7

/** 视图名称最大长度 */
#define CLI_CLI_MAX_VIEW_LEN 20
/** 视图最大长度 */
#define CLI_MAX_VIEW_LEN 64
/** 提示符最大长度 */
#define CLI_CLI_MAX_PROMPT_LEN 128

/**
 * 上下文 TLV 类型字节（type 字段）。
 * 值 0x80/0x81 不与 DB_TYPE_* (0-4) 冲突。
 * 整数上下文条目格式: [ctx_id:u32][CLI_TLV_TYPE_CTX:u8][4:u16][u32_value]
 * 字符串上下文条目格式: [ctx_id:u32][CLI_TLV_TYPE_CTX_STR:u8][len:u16][string_bytes]
 * 命令参数条目格式: [cfg_id:u32][DB_TYPE_*:u8][len:u16][value...]
 */
#define CLI_TLV_TYPE_CTX 0x80

/** 字符串上下文 TLV 类型字节（用于存储 VRF 名称等字符串型上下文参数） */
#define CLI_TLV_TYPE_CTX_STR 0x81

/** 判断 TLV 条目是否为上下文变量（整数或字符串类型均属于上下文） */
#define CLI_TLV_IS_CTX(entry_ptr) ((entry_ptr)->type == CLI_TLV_TYPE_CTX || (entry_ptr)->type == CLI_TLV_TYPE_CTX_STR)

// ============================================================================
// TLV 载荷格式定义
// ============================================================================

/** 载荷 flags: "no" 前缀命令（删除操作） */
#define CLI_PAYLOAD_FLAG_NO_CMD 0x01
/** 载荷 flags: show 命令 */
#define CLI_PAYLOAD_FLAG_SHOW_CMD 0x02

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
 * @brief CLI 文本分片流状态（用于 RESP_MORE / CONTINUE）
 */
typedef struct cli_chunk_stream
{
    GString *full_text;     /**< 完整待输出文本 */
    gsize offset;           /**< 已发送偏移 */
    uint32_t src_module_id; /**< 当前分片所属请求源模块 ID */
} cli_chunk_stream_t;

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
 * @brief 从 TLV 条目中读取整数值
 * @param entry TLV 条目
 * @return 整数值，类型不匹配返回 0
 */
int64_t cli_tlv_entry_get_int(const cli_tlv_entry_t *entry);

/**
 * @brief 从 CTX TLV 条目中读取 uint32 值（type==CLI_TLV_TYPE_CTX，len==4）
 * @param entry TLV 条目
 * @return 无符号整数值，类型/长度不匹配返回 0
 */
uint32_t cli_tlv_entry_get_ctx_uint32(const cli_tlv_entry_t *entry);

/**
 * @brief 从 TLV 条目中读取字符串值
 * @param entry TLV 条目
 * @return 字符串（内部指针，不转移所有权），类型不匹配返回 NULL
 */
const char *cli_tlv_entry_get_text(const cli_tlv_entry_t *entry);

/**
 * @brief 重置并释放分片流状态
 * @param stream 分片流状态
 */
void cli_chunk_stream_reset(cli_chunk_stream_t *stream);

/**
 * @brief 启动分片输出（必要时首包发送 RESP_MORE）
 * @param stream 分片流状态
 * @param ctx IPC 上下文
 * @param self_module_id 本模块 ID
 * @param req 原始 CLI 请求消息（用于回填 dst/request_id）
 * @param full_text 完整输出文本（函数接管所有权，可为 NULL）
 * @return ERRCODE_SUCCESS/ERRCODE_FAIL
 */
int cli_chunk_stream_start(cli_chunk_stream_t *stream, dev_ipc_context_t *ctx, uint32_t self_module_id,
                           const dev_ipc_message_t *req, GString *full_text);

/**
 * @brief 处理 CLI_MSG_TYPE_CONTINUE，发送下一片
 * @param stream 分片流状态
 * @param ctx IPC 上下文
 * @param self_module_id 本模块 ID
 * @param req CONTINUE 请求消息
 * @return ERRCODE_SUCCESS/ERRCODE_FAIL
 */
int cli_chunk_stream_continue(cli_chunk_stream_t *stream, dev_ipc_context_t *ctx, uint32_t self_module_id,
                              const dev_ipc_message_t *req);

#endif // CLI_H
