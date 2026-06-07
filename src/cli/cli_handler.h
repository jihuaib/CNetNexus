/**
 * @file   cli_handler.h
 * @brief  CLI 客户端会话管理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_HANDLER_H
#define CLI_HANDLER_H

#include <glib.h>
#include <stdint.h>
#include <time.h>

#include "cli_history.h"
#include "cli_view.h"

#define CLI_PROMPT_STACK_DEPTH 8
#define CLI_PAGER_DEFAULT_LINES 24

// 逻辑会话结构（ACCESS 架构）
//
// CLI 是纯命令引擎：以 line_id（ACCESS 线号）为键维护逻辑会话，只持有命令解析所需的
// 视图/上下文状态；终端态（行编辑/光标/ANSI/历史/pager/socket）全部在 ACCESS line 层。
typedef struct
{
    cli_view_node_t *current_view;       // 当前视图节点
    char prompt[CLI_CLI_MAX_PROMPT_LEN]; // 当前提示符模板（渲染前）
    char client_ip[MAX_CLIENT_IP_LEN];   // 客户端地址（用于 show client / 全局历史）
    uint16_t client_port;                // 客户端源端口
    time_t connect_time;                 // 建连时刻（秒）

    uint32_t line_id;            // ACCESS 线号（= session_id）
    GString *out;                // 命令输出缓冲（cli_send_* 累积到此，经 IPC 回传 ACCESS）
    GString *access_out_pending; // LINE_INPUT 超长输出的待发送分片缓存
    gsize access_out_offset;     // access_out_pending 已发送偏移
    uint32_t close_requested;    // 1=命令要求关闭本会话（顶层 exit）

    // ACCESS line 层本地命令：CLI 匹配到 module=ACCESS 的命令时填这两个字段，
    // 经 INPUT_RESP 回传给 ACCESS 本地执行（bash/terminal length），不走 IPC 分发。
    uint32_t line_cmd;    // 见 ACCESS_LINE_CMD_*，0=无
    uint32_t line_cmd_no; // 是否带 no 前缀
    uint32_t line_cmd_arg1; // line 命令参数1（如 transport input 的 vty 起始线号，取自 line 视图上下文）
    uint32_t line_cmd_arg2; // line 命令参数2（vty 结束线号）

    // 提示符栈：进入子视图前保存上层提示符
    char prompt_stack[CLI_PROMPT_STACK_DEPTH][CLI_CLI_MAX_PROMPT_LEN];
    uint32_t prompt_stack_depth;

    // 视图上下文栈：保存进入子视图时模块设置的环境变量
    uint8_t *view_context_stack[CLI_PROMPT_STACK_DEPTH]; // 每层上下文 TLV 数据
    uint32_t view_context_len[CLI_PROMPT_STACK_DEPTH];   // 每层数据长度
} cli_session_t;

// Function prototypes
void cli_cleanup(void);
void cli_session_destroy(cli_session_t *session);
void send_prompt(cli_session_t *session);
void cli_render_prompt(cli_session_t *session, char *out, size_t out_size);
void update_prompt_from_template(cli_session_t *session, const char *module_prompt);
void cli_prompt_push(cli_session_t *session);
void cli_prompt_pop(cli_session_t *session);
void cli_context_set(cli_session_t *session, const uint8_t *data, uint32_t len);
const uint8_t *cli_context_get(cli_session_t *session, uint32_t *out_len);
void cli_send_message(cli_session_t *session, const char *message);
void cli_send_data(cli_session_t *session, const void *data, size_t len);
int process_command(const char *cmd_line, cli_session_t *session);
void cli_build_tab_candidates(cli_session_t *session, const char *partial, GString *out);
void cli_build_help_text(cli_session_t *session, const char *partial, GString *out);
void cli_pager_output(cli_session_t *session, const char *message);

#endif // CLI_HANDLER_H
