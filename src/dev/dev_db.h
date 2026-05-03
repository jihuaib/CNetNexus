/**
 * @file   dev_db.h
 * @brief  DEV 模块配置持久化（log-level 等）
 * @author jhb
 * @date   2026/04/27
 */
#ifndef DEV_DB_H
#define DEV_DB_H

#include <stdint.h>

#include "log.h"

/** DEV 配置表名（单行，存放 log_level 等运行时配置） */
#define DEV_TABLE_CONFIG "dev_config"

/** 单行配置主键值 */
#define DEV_CONFIG_PK_VALUE 1

/**
 * @brief 获取构建版本对应的缺省日志级别
 * @return Release 构建返回 LOG_LEVEL_WARN，Debug 构建返回 LOG_LEVEL_DEBUG
 */
log_level_t dev_db_default_log_level(void);

/**
 * @brief 初始化 DEV 配置表（建表，已存在则幂等跳过）
 * @return 0 成功，-1 失败
 */
int dev_db_init(void);

/**
 * @brief 从 DB 恢复 DEV 配置：读取并应用 log_level（无记录则按构建版本写入缺省值）
 * @return 0 成功，-1 失败
 */
int dev_db_restore(void);

/**
 * @brief 写入（插入或更新）log_level 配置
 * @param level 日志级别
 * @return 0 成功，-1 失败
 */
int dev_db_set_log_level(log_level_t level);

/**
 * @brief 读取已存日志级别
 * @param[out] level 输出日志级别
 * @return 0 成功（有记录），1 无记录，-1 失败
 */
int dev_db_get_log_level(log_level_t *level);

/**
 * @brief 读取持久化 sysname
 * @return 0=已读取（out 可能为空字符串）, 1=DB 中不存在, -1=失败
 */
int dev_db_get_sysname(char *out, size_t cap);

/**
 * @brief 持久化 sysname；NULL 或空串视为清空（恢复默认）
 * @return 0 成功，-1 失败
 */
int dev_db_set_sysname(const char *sysname);

#endif /* DEV_DB_H */
