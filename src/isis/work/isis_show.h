/**
 * @file   isis_show.h
 * @brief  ISIS show 命令处理（worker 线程）
 * @author jhb
 * @date   2026/04/12
 */
#ifndef ISIS_SHOW_H
#define ISIS_SHOW_H

#include "dev.h"

int isis_show_handle_msg(dev_ipc_message_t *msg);
void isis_show_cleanup(void);

#endif /* ISIS_SHOW_H */
