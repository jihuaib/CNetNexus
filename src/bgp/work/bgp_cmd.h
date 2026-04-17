/**
 * @file   bgp_cmd.h
 * @brief  BGP worker 命令队列（外部线程 -> worker 线程的 RPC 通道）
 * @author jhb
 * @date   2026/04/17
 *
 * 本文件只导出 worker 主循环与 channel init/cleanup 所需的集成点。
 * 命令结构（bgp_cmd_t）、类型枚举、静态队列操作函数均为 bgp_cmd.c 内部实现，
 * 对外不可见。具体的外部线程投递 API（bgp_worker_post_*, bgp_worker_dispatch_apply）
 * 定义在 bgp_worker.h 中保持稳定，实现在 bgp_cmd.c。
 */
#ifndef BGP_CMD_H
#define BGP_CMD_H

#include <glib.h>

/**
 * @brief epoll data.ptr sentinel：区分 worker 命令 eventfd 事件
 *
 * worker 主循环通过指针比较识别命令事件并调用 bgp_cmd_process_event。
 */
extern char bgp_cmd_tag;

/**
 * @brief 处理 worker 命令队列中的所有待处理命令（仅 worker 线程调用）
 *
 * 由主循环在 cmd_eventfd 触发时调用。一次性排干 eventfd 计数并
 * 依次分发队列中所有命令（SHOW_CLI / ROUTE_MSG / IF_EVENT / APPLY / SHUTDOWN）。
 *
 * @return TRUE 表示收到 SHUTDOWN 命令，主循环应退出；FALSE 继续运行
 */
gboolean bgp_cmd_process_event(void);

/**
 * @brief 排干命令队列中所有待处理命令，将它们标记为失败并释放资源
 *
 * 在 worker 线程退出或 channel cleanup 时调用：
 * - waitable 命令（如 APPLY）回写失败码以唤醒等待线程
 * - 非 waitable 命令直接销毁
 * - 附带的 dev_ipc_message_t 统一释放
 */
void bgp_cmd_drain_queue(void);

/**
 * @brief 向 worker 线程同步投递 SHUTDOWN 命令并阻塞等待完成
 *
 * 由 bgp_worker_shutdown 使用。成功投递后阻塞直到 worker 线程处理完毕；
 * 若投递失败，作为兜底直接置 g_bgp_work_local->running=0。
 */
void bgp_cmd_post_shutdown(void);

#endif /* BGP_CMD_H */
