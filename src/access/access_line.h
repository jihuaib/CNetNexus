/**
 * @file   access_line.h
 * @brief  ACCESS 线池与线（终端）管理：vty 线池、终端态、行编辑/ANSI/pager
 * @author jhb
 * @date   2026/05/30
 *
 * 一条"线"对应一个接入会话（telnet→vty）。线号即 session_id，传给 CLI 作逻辑会话键。
 * 线持有全部终端态（行缓冲/光标/ANSI 状态机/历史/pager），不持有命令树/视图（在 CLI）。
 */
#ifndef ACCESS_LINE_H
#define ACCESS_LINE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "access.h"
#include "access_history.h"

/** console 专用线槽（线号 0，永远为串口/console 保留，不被 vty 占用） */
#define ACCESS_CON_LINE_ID 0
/** vty 线起始线号 */
#define ACCESS_VTY_FIRST_ID 1
/** 线池容量：1 条 con（线 0）+ 5 条 vty（线 1..5），对标主流厂商默认 vty 0 4 */
#define ACCESS_LINE_POOL_SIZE 6
/** vty 线条数 */
#define ACCESS_VTY_COUNT (ACCESS_LINE_POOL_SIZE - ACCESS_VTY_FIRST_ID)

/** transport input 协议位（per vty 线，默认 0=none） */
#define ACCESS_TRANSPORT_TELNET 0x1u
#define ACCESS_TRANSPORT_SSH 0x2u
/** 每屏默认行数（pager length 属性默认值） */
#define ACCESS_PAGER_DEFAULT_LINES 24

/** epoll data.ptr 指向对象的类别（监听 socket vs 客户端线），均以此为首字段 */
typedef enum
{
    ACCESS_EP_LINE = 0, /**< access_line_t* */
    ACCESS_EP_LISTENER, /**< access_listener_t* */
} access_ep_kind_t;

/** 输入状态机：用于解析 ANSI 转义序列 */
typedef enum
{
    ACCESS_INPUT_NORMAL,
    ACCESS_INPUT_ESC,
    ACCESS_INPUT_CSI,
} access_input_state_t;

/** 一条线（接入会话）的完整终端态 */
typedef struct access_line
{
    access_ep_kind_t ep_kind; /**< 恒为 ACCESS_EP_LINE（必须是首字段，供 epoll 判别） */
    uint32_t line_id;         /**< 线号（=池下标，=session_id） */
    uint16_t line_type;       /**< 线类型，见 ACCESS_LINE_TYPE_* */
    int in_use;               /**< 1=已分配 */
    int fd;                   /**< 客户端 socket */
    uint32_t generation;      /**< 线槽复用代次，防止旧桥接线程误操作新会话 */

    char client_ip[ACCESS_MAX_CLIENT_IP_LEN]; /**< 客户端地址 */
    uint16_t client_port;                     /**< 客户端源端口 */
    time_t connect_time;                      /**< 建连时刻 */

    char prompt[ACCESS_PROMPT_MAX_LEN]; /**< CLI 下发的已渲染提示符 */

    char line_buffer[ACCESS_MAX_CMD_LEN]; /**< 当前行缓冲 */
    uint32_t line_pos;                    /**< 行长度 */
    uint32_t cursor_pos;                  /**< 光标位置 */
    access_input_state_t state;           /**< 输入状态机状态 */

    uint32_t tab_cycling;                  /**< 1=正在循环 Tab 候选 */
    uint32_t tab_match_index;              /**< 当前候选索引 */
    char tab_original[ACCESS_MAX_CMD_LEN]; /**< 循环前的原始输入 */
    uint32_t tab_original_pos;             /**< 循环前的光标位置 */

    access_session_history_t history; /**< 本线命令历史 */

    char *pager_buffer;            /**< pager 输出缓冲 */
    uint32_t pager_offset;         /**< pager 当前位置 */
    uint32_t pager_total_len;      /**< pager 缓冲总长 */
    uint32_t pager_lines_per_page; /**< 每屏行数（line length 属性，0=禁用分页） */
    uint32_t pager_active;         /**< 1=pager 激活中 */

    uint32_t close_requested; /**< 1=命令要求关闭本线（顶层 exit） */
    uint32_t enter_bash;      /**< 1=本行是 bash 命令，server 线程应在本线 fd 上桥接 PTY */
    uint32_t bash_active;     /**< 1=本线 fd 正由 bash PTY 桥接线程接管 */
} access_line_t;

// ============================================================================
// 线池 API
// ============================================================================

/** @brief 初始化线池与全局历史 */
void access_line_pool_init(void);
/** @brief 销毁线池（关闭所有线、释放资源） */
void access_line_pool_cleanup(void);

/**
 * @brief 分配一条空闲线
 * @param fd        客户端 socket
 * @param ip        客户端地址字符串
 * @param port      客户端源端口
 * @param line_type 线类型（ACCESS_LINE_TYPE_CON 用 console 专用槽；VTY 用 vty 池）
 * @return 线指针；对应池满返回 NULL
 */
access_line_t *access_line_alloc(int fd, const char *ip, uint16_t port, uint16_t line_type);

/** @brief 释放一条线（关闭 fd、清理历史/pager） */
void access_line_free(access_line_t *line);

/** @brief 按线号查找线 */
access_line_t *access_line_find(uint32_t line_id);

/** @brief 设置 vty 线区间 [first,last]（vty_num）的 transport input 为 bits（replace 语义） */
void access_vty_set_transport(uint32_t first, uint32_t last, uint8_t bits);
/** @brief 是否有任一 vty 线开启 telnet */
int access_vty_any_telnet(void);
/** @brief 全局 telnet server 是否使能（telnet 23 监听门控；与 per-line transport 独立） */
int access_telnet_server_enabled(void);

/** @brief 设置全局 telnet server 使能标志（不触发监听起停，供 DB restore 用） */
void access_set_telnet_server_enabled(int enabled);

/**
 * @brief 无终端内部会话应用持久化 ACCESS 配置命令
 * @return 0 成功，-1 表示该命令不是可回放的持久化配置
 */
int access_apply_config_command(uint32_t line_cmd, uint32_t line_cmd_no, uint32_t arg1, uint32_t arg2);

// ============================================================================
// 终端交互 API
// ============================================================================

/**
 * @brief 读取并处理本线可用输入（喂入状态机，本地完成回显/行编辑/分页）
 * @return 0 正常；-1 表示连接断开/出错，调用方应释放本线
 */
int access_line_process_input(access_line_t *line);

/** @brief 向本线发送字符串（null 结尾） */
void access_line_send(access_line_t *line, const char *msg);
/** @brief 按线号发送字符串（null 结尾），供非 line 线程输出 */
void access_line_send_to(uint32_t line_id, const char *msg);
/** @brief 向本线发送原始数据 */
void access_line_send_data(access_line_t *line, const void *data, size_t len);

/** @brief 发送 telnet 协商并向 CLI 建立逻辑会话（建连后调用） */
void access_line_greet(access_line_t *line);

/** @brief 发送当前提示符（CLI 下发的已渲染串 + 尾随空格） */
void access_line_send_prompt(access_line_t *line);

/** @brief 向 CLI 发 SESSION_CLOSE 销毁逻辑会话（线断开时调用） */
void access_line_close_on_cli(uint32_t line_id);

#endif // ACCESS_LINE_H
