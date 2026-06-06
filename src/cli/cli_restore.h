/**
 * @file   cli_restore.h
 * @brief  CLI 内部配置回放入口
 */
#ifndef CLI_RESTORE_H
#define CLI_RESTORE_H

/**
 * @brief 冷启动后等待 DEV READY，并在 startup/cfg 模式下回放对应的 BDR cfg 文本
 */
void cli_restore_startup_if_needed(void);

#endif // CLI_RESTORE_H
