/**
 * @file   dev_fs_cli.h
 * @brief  DEV filesystem CLI commands (ls/cd/more)
 * @author jhb
 * @date   2026/06/06
 */
#ifndef DEV_FS_CLI_H
#define DEV_FS_CLI_H

#include "cli.h"
#include "dev.h"

int dev_fs_cli_handle_ls(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser);
int dev_fs_cli_handle_cd(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser);
int dev_fs_cli_handle_more(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser);
int dev_fs_cli_handle_pwd(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser);
void dev_fs_cli_cleanup_line(uint32_t line_id);
void dev_fs_cli_cleanup_all(void);

#endif // DEV_FS_CLI_H
