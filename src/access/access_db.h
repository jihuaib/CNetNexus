/**
 * @file   access_db.h
 * @brief  ACCESS 配置持久化（telnet server 开关 + per-vty transport + console 线默认行）
 * @author jhb
 * @date   2026/05/30
 */
#ifndef ACCESS_DB_H
#define ACCESS_DB_H

#include <glib.h>
#include <stdint.h>

#include "cli.h"

/** @brief 建表（access_setting / access_line）+ 首次启动写入默认行（含 console 线） */
int access_db_init(void);

/** @brief 从 DB 恢复配置到内存（telnet server 使能 + 各 vty transport）。调用方随后 apply_gating */
int access_db_restore(void);

/** @brief 持久化全局 telnet server 使能标志 */
int access_db_save_telnet_server(int enabled);

/** @brief 持久化某条 vty 线的 transport input 位掩码 */
int access_db_save_vty_transport(uint32_t vty_num, uint8_t transport);

/** @brief 从 DB 构建 show running-config 的 ACCESS 配置块（telnet server / line console / line vty） */
void access_db_build_running_config(GString *out);

/** @brief 从 DB 构建当前 ACCESS 视图的 show this 配置块 */
void access_db_build_running_config_scoped(GString *out, const cli_show_scope_t *scope);

#endif // ACCESS_DB_H
