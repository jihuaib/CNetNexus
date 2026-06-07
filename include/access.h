/**
 * @file   access.h
 * @brief  ACCESS 接入层（line 层）与 CLI 命令引擎之间的 IPC 契约
 * @author jhb
 * @date   2026/05/30
 *
 * 架构：ACCESS 是 line 层，按主流厂商 user-interface/line 模型管理接入通道
 *       （telnet→vty，后续 con/tty/ssh），持有终端态（回显/行编辑/ANSI/pager/历史）。
 *       CLI 是纯命令引擎，按 line_id 维护逻辑会话，对 ACCESS 提供命令解析/补全/帮助的
 *       RPC 应答。所有消息均为 ACCESS 发起的 query、CLI 应答的 response。
 */
#ifndef ACCESS_H
#define ACCESS_H

#include <stdint.h>
#include <stdlib.h>

#include "dev.h"

/** console（串口）通道的 unix socket 路径：环境变量 NN_CONSOLE_SOCK 优先，否则默认 /tmp。
 *  ACCESS 监听端与 netnexus-console 客户端共用此解析。 */
static inline const char *access_console_sock_path(void)
{
    const char *p = getenv("NN_CONSOLE_SOCK");
    return (p && p[0] != '\0') ? p : "/tmp/netnexus-console.sock";
}

// ============================================================================
// ACCESS 消息类型定义（大类 = DEV_IPC_CATEGORY_ACCESS）
// ============================================================================

/** ACCESS→CLI：新线接入，请求建立逻辑会话；CLI 回 ACCESS_MSG_OPEN_RESP */
#define ACCESS_MSG_SESSION_OPEN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0001)
/** ACCESS→CLI：一整行命令就绪，请求执行；CLI 回 ACCESS_MSG_INPUT_RESP */
#define ACCESS_MSG_LINE_INPUT DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0002)
/** ACCESS→CLI：Tab 补全请求；CLI 回 ACCESS_MSG_TAB_RESP */
#define ACCESS_MSG_TAB_REQ DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0003)
/** ACCESS→CLI：'?' 帮助请求；CLI 回 ACCESS_MSG_HELP_RESP */
#define ACCESS_MSG_HELP_REQ DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0004)
/** ACCESS→CLI：线断开，请求销毁逻辑会话；CLI 回空 ACCESS_MSG_CLOSE_RESP */
#define ACCESS_MSG_SESSION_CLOSE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0005)
/** ACCESS→CLI：继续拉取 LINE_INPUT 的分片响应，payload=uint32_t line_id；CLI 回 ACCESS_MSG_INPUT_RESP */
#define ACCESS_MSG_INPUT_CONTINUE DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0006)

/** CLI→ACCESS：SESSION_OPEN 响应（access_text_resp_t，text=welcome 文本） */
#define ACCESS_MSG_OPEN_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0081)
/** CLI→ACCESS：LINE_INPUT 响应（access_text_resp_t，text=命令输出） */
#define ACCESS_MSG_INPUT_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0082)
/** CLI→ACCESS：TAB_REQ 响应（access_tab_resp_t） */
#define ACCESS_MSG_TAB_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0083)
/** CLI→ACCESS：HELP_REQ 响应（裸文本，null 结尾） */
#define ACCESS_MSG_HELP_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0084)
/** CLI→ACCESS：SESSION_CLOSE 响应（空 payload） */
#define ACCESS_MSG_CLOSE_RESP DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0085)
/** CLI→ACCESS：LINE_INPUT 执行期间的单向进度输出（access_line_progress_t） */
#define ACCESS_MSG_LINE_PROGRESS DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x0086)

/** ACCESS 内部消息：DB READY（DB 事件回调投递到 worker 线程做建表/恢复，回调本身不能阻塞） */
#define ACCESS_MSG_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ACCESS, 0x00F1)

// ============================================================================
// 常量
// ============================================================================

/** 提示符字符串最大长度（与 CLI 渲染保持一致） */
#define ACCESS_PROMPT_MAX_LEN 128
/** 客户端地址字符串最大长度 */
#define ACCESS_CLIENT_IP_LEN 64

/** 线类型 */
enum
{
    ACCESS_LINE_TYPE_VTY = 0, /**< 虚拟终端线（telnet/ssh 远程接入） */
    ACCESS_LINE_TYPE_CON = 1, /**< 控制台线（本地 console） */
    ACCESS_LINE_TYPE_TTY = 2, /**< 异步串口线 */
};

/** access_text_resp_t.flags 位定义 */
#define ACCESS_RESP_FLAG_CLOSE_SESSION (1U << 0) /**< 命令要求关闭本线（如顶层 exit） */
#define ACCESS_RESP_FLAG_MORE (1U << 1)          /**< 本响应后还有更多 text 分片，ACCESS 需继续拉取 */

/** ACCESS line 层本地命令（在 ACCESS 的 commands.xml 注册，module-id=ACCESS；CLI 匹配后回传
 *  group 让 ACCESS 本地执行，不走 IPC 分发）。取值即 ACCESS commands.xml 的 group-id。 */
enum
{
    ACCESS_LINE_CMD_NONE = 0,            /**< 非 line 命令（普通命令，已由 CLI 处理） */
    ACCESS_LINE_CMD_BASH = 1,            /**< bash：进入 shell */
    ACCESS_LINE_CMD_TERMINAL_LENGTH = 2, /**< terminal length 0 / no terminal length 0 */
    ACCESS_LINE_CMD_LINE_ENTER = 3, /**< line vty/console：进入 line 视图（视图切换由 CLI 完成，ACCESS no-op） */
    ACCESS_LINE_CMD_TRANSPORT_TELNET = 4, /**< transport input telnet */
    ACCESS_LINE_CMD_TRANSPORT_SSH = 5,    /**< transport input ssh（占位，暂不支持） */
    ACCESS_LINE_CMD_TRANSPORT_ALL = 6,    /**< transport input all */
    ACCESS_LINE_CMD_TRANSPORT_NONE = 7,   /**< transport input none / no transport input */
    ACCESS_LINE_CMD_TELNET_SERVER = 8,    /**< telnet server enable / no telnet server enable（全局监听开关） */
    ACCESS_LINE_CMD_SHOW_LINE = 9,        /**< show line（显示各线类型/transport/状态） */
};

/** line 视图上下文 ID（全局唯一，避开 BGP 的 2/5/7/10/11/12） */
#define ACCESS_CTX_ID_LINE_FIRST 20 /**< vty 起始线号 */
#define ACCESS_CTX_ID_LINE_LAST 21  /**< vty 结束线号 */

/** access_tab_resp_t.kind 取值 */
enum
{
    ACCESS_TAB_KIND_NONE = 0,   /**< 无匹配 */
    ACCESS_TAB_KIND_TOKENS = 1, /**< 候选 token 列表（data 双 null 结尾） */
};

// ============================================================================
// 载荷结构体
// ============================================================================

/**
 * @brief SESSION_OPEN 请求载荷
 */
typedef struct access_session_open
{
    uint32_t line_id;                     /**< 线号（即 session_id） */
    uint16_t line_type;                   /**< 线类型，见 ACCESS_LINE_TYPE_* */
    uint16_t client_port;                 /**< 客户端源端口 */
    char client_ip[ACCESS_CLIENT_IP_LEN]; /**< 客户端地址字符串 */
} access_session_open_t;

/**
 * @brief LINE_INPUT / TAB_REQ / HELP_REQ / SESSION_CLOSE 通用请求头
 *        cmdline 为变长字段：LINE_INPUT 为整行命令，TAB/HELP 为光标前文本，
 *        SESSION_CLOSE 时为空。
 */
typedef struct access_line_input
{
    uint32_t line_id; /**< 线号 */
    char cmdline[];   /**< 变长，null 结尾 */
} access_line_input_t;

/**
 * @brief OPEN_RESP / INPUT_RESP 通用文本响应
 *        text 为变长字段：OPEN_RESP 为 welcome，INPUT_RESP 为命令输出。
 */
typedef struct access_text_resp
{
    uint32_t flags;       /**< 见 ACCESS_RESP_FLAG_* */
    uint32_t line_cmd;    /**< 非 0=本行是 ACCESS line 命令，由 ACCESS 本地执行，见 ACCESS_LINE_CMD_* */
    uint32_t line_cmd_no; /**< line 命令是否带 no 前缀（1=有，如 no terminal length 0） */
    uint32_t line_arg1; /**< line 命令参数 1（如 transport input 的 vty 起始线号，来自 line 视图上下文） */
    uint32_t line_arg2;                 /**< line 命令参数 2（如 vty 结束线号） */
    char prompt[ACCESS_PROMPT_MAX_LEN]; /**< 渲染好的当前提示符 */
    char text[];                        /**< 变长，null 结尾 */
} access_text_resp_t;

/**
 * @brief LINE_INPUT 执行期间的单向进度输出
 */
typedef struct access_line_progress
{
    uint32_t line_id; /**< 线号 */
    char text[];      /**< 变长，null 结尾 */
} access_line_progress_t;

/**
 * @brief TAB_RESP 响应
 *        kind=TOKENS 时 data 为 "tok1\0tok2\0\0" 双 null 结尾的候选列表，
 *        由 ACCESS 本地完成唯一补全/列表显示/循环。
 */
typedef struct access_tab_resp
{
    uint32_t kind; /**< 见 ACCESS_TAB_KIND_* */
    char data[];   /**< 变长候选列表 */
} access_tab_resp_t;

#endif // ACCESS_H
