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
/** DEV → CFG：更新系统名（payload: 字符串，null 结尾；空字符串=恢复默认 "NetNexus"） */
#define CLI_MSG_TYPE_SYSNAME_UPDATE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0009)

/** 系统名最大长度（含 null） */
#define CLI_SYSNAME_MAX_LEN 64
/** 默认系统名 */
#define CLI_SYSNAME_DEFAULT "NetNexus"

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
/** null0 接口视图 */
#define CLI_VIEW_IF_NULL0 "if-null0"
/** loop 接口视图 */
#define CLI_VIEW_IF_LOOP "if-loop"
/** 路由视图 */
#define CLI_VIEW_ROUTE "route"
/** ISIS 配置视图 */
#define CLI_VIEW_ISIS "isis"
/** LDP 配置视图 */
#define CLI_VIEW_LDP "ldp"
/** BGP 地址族 IPv4 单播视图 */
#define CLI_VIEW_BGP_AF_IPV4 "bgp-af-ipv4-uni"
/** BGP 地址族 IPv6 单播视图 */
#define CLI_VIEW_BGP_AF_IPV6 "bgp-af-ipv6-uni"
/** BGP 地址族 IPv4 QP 视图 */
#define CLI_VIEW_BGP_AF_IPV4_QP "bgp-af-ipv4-qp"
/** BGP 地址族 IPv6 QP 视图 */
#define CLI_VIEW_BGP_AF_IPV6_QP "bgp-af-ipv6-qp"
/** BGP 地址族 IPv4 labeled-unicast 视图 */
#define CLI_VIEW_BGP_AF_IPV4_LABELED "bgp-af-ipv4-labeled"
/** BGP 地址族 IPv6 labeled-unicast 视图 */
#define CLI_VIEW_BGP_AF_IPV6_LABELED "bgp-af-ipv6-labeled"
/** BGP BMP 实例视图 */
#define CLI_VIEW_BGP_BMP "bgp-bmp"
/** VRF 配置视图 */
#define CLI_VIEW_VRF "vrf"
/** VRF IPv4 单播地址族视图 */
#define CLI_VIEW_VRF_AF_IPV4 "vrf-af-ipv4-uni"
/** VRF IPv6 单播地址族视图 */
#define CLI_VIEW_VRF_AF_IPV6 "vrf-af-ipv6-uni"

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
/** ISIS 实例标签上下文（整数） */
#define CLI_CTX_ID_ISIS_TAG 8
/** VRF 视图当前 AF AFI 上下文（整数：VRF_AFI_*） */
#define CLI_CTX_ID_VRF_AFI 9
/** VRF 视图当前 AF SAFI 上下文（整数：VRF_SAFI_*） */
#define CLI_CTX_ID_VRF_SAFI 10
/** LDP 视图占位上下文（无字段，单实例 LDP；保留给将来多实例使用） */
#define CLI_CTX_ID_LDP_INST 11

/** 视图名称最大长度 */
#define CLI_CLI_MAX_VIEW_LEN 20
/** 视图最大长度 */
#define CLI_MAX_VIEW_LEN 64
/** 提示符最大长度 */
#define CLI_CLI_MAX_PROMPT_LEN 128

/**
 * @brief show 配置收集作用域模式
 */
typedef enum cli_show_scope_mode
{
    CLI_SHOW_SCOPE_MODE_FULL = 0, /**< 全量配置（show current-configuration） */
    CLI_SHOW_SCOPE_MODE_THIS = 1, /**< 当前视图配置（show this） */
} cli_show_scope_mode_t;

/**
 * @brief SHOW_CONFIG 可选作用域描述
 *
 * payload 为空时等价于 FULL 模式。
 * ctx_data 指向 payload 内部切片，不转移所有权。
 */
typedef struct cli_show_scope
{
    cli_show_scope_mode_t mode;
    char view_name[CLI_MAX_VIEW_LEN];
    const uint8_t *ctx_data;
    uint32_t ctx_len;
} cli_show_scope_t;

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
 * @brief 将 TLV 解析器重置到首个条目位置（保留 flags/group_id）
 * @param p 解析器
 */
void cli_tlv_rewind(cli_tlv_parser_t *p);

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
 * @brief 从 TLV 条目中读取 uint32 值
 * @param entry TLV 条目
 * @param out_value 输出值
 * @return 0=成功，-1=类型/长度不匹配
 */
int cli_tlv_entry_get_u32(const cli_tlv_entry_t *entry, uint32_t *out_value);

/**
 * @brief 从 CTX TLV 条目中读取 uint32 值（type==CLI_TLV_TYPE_CTX，len==4）
 * @param entry TLV 条目
 * @return 无符号整数值，类型/长度不匹配返回 0
 */
uint32_t cli_tlv_entry_get_ctx_uint32(const cli_tlv_entry_t *entry);

/**
 * @brief 在原始上下文 TLV 缓冲中查找某个 uint32 上下文变量
 * @param ctx_data 上下文 TLV 原始字节（格式: [num:u16][ctx_id:u32][type:u8][len:u16][value...]...）
 * @param ctx_len  缓冲长度
 * @param ctx_id   要查找的上下文 ID
 * @param out_value 输出值
 * @return 0=找到，-1=未找到或格式不匹配
 */
int cli_ctx_lookup_uint32(const uint8_t *ctx_data, uint32_t ctx_len, uint32_t ctx_id, uint32_t *out_value);

/**
 * @brief 在原始上下文 TLV 缓冲中查找某个字符串上下文变量
 * @param ctx_data 上下文 TLV 原始字节（格式: [num:u16][ctx_id:u32][type:u8][len:u16][value...]...）
 * @param ctx_len  缓冲长度
 * @param ctx_id   要查找的上下文 ID
 * @param out_buf  输出缓冲
 * @param out_cap  输出缓冲大小
 * @return 0=找到，-1=未找到或格式不匹配
 */
int cli_ctx_lookup_text(const uint8_t *ctx_data, uint32_t ctx_len, uint32_t ctx_id, char *out_buf, size_t out_cap);

/**
 * @brief 构造 SHOW_CONFIG 作用域 payload
 * payload 格式: [mode:u8][view_len:u16][view_bytes][ctx_len:u32][ctx_bytes]
 * @param scope   作用域描述
 * @param out_len 输出 payload 长度
 * @return 新分配的 payload（g_free 释放）；若 scope 为空或等价于 FULL 无附加信息则返回 NULL
 */
uint8_t *cli_show_scope_payload_build(const cli_show_scope_t *scope, uint32_t *out_len);

/**
 * @brief 解析 SHOW_CONFIG 作用域 payload
 * @param payload     原始 payload，可为 NULL
 * @param payload_len payload 长度
 * @param scope_out   输出作用域；payload 为空时返回 FULL 模式
 * @return 0=成功，-1=格式非法
 */
int cli_show_scope_payload_parse(const uint8_t *payload, uint32_t payload_len, cli_show_scope_t *scope_out);

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

// ============================================================================
// show current-configuration 通用配置锚点 (Config Anchor) 框架
//
// 设计目的:
//   - 多个模块可能同时对同一"配置容器"贡献内嵌行, 例如接口、VRF、route-policy
//     等。本框架把"容器"抽象成不透明的 anchor key, 让 CLI 聚合器按 key 合并
//     各模块的贡献, 避免同一容器被多次重复输出。
//   - CLI 框架对 anchor key 的含义完全不关心, 不含任何业务词汇; 业务模块之间
//     通过约定的 key 字符串互相配合。
//
// 典型用法:
//   - 属主模块 (owner, 例如 IF 对接口): 发射 header + 0..N 条 body + footer
//   - 贡献者模块 (contributor, 例如 ISIS 对接口): 只发射 body
//   - 与 anchor 无关的顶层配置: 用 cli_cfg_anchor_emit_global 原样追加
// ============================================================================

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

#endif // CLI_H
