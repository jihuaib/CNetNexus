/**
 * @file   sbmp_db.h
 * @brief  SBMP 模块数据库操作接口
 * @author jhb
 * @date   2026/03/08
 */
#ifndef SBMP_DB_H
#define SBMP_DB_H

#include <stdint.h>

#include "db.h"
#include "dev.h"

/** SBMP Server 配置表名 */
#define SBMP_TABLE_SERVER "sbmp_server"

/**
 * @brief 初始化 SBMP 数据库（建表，已存在则幂等跳过）
 * @return 0 成功，-1 失败
 */
int sbmp_db_init(void);

/**
 * @brief 从数据库恢复 SBMP 状态
 * @return 0 成功，-1 失败
 */
int sbmp_db_restore(void);

/**
 * @brief 写入（插入或更新）BMP server 监听端口
 * @param port 监听端口（1-65535）
 * @return 0 成功，-1 失败
 */
int sbmp_db_set_server_port(uint16_t port);

/**
 * @brief 从数据库删除 BMP server 配置
 * @return 删除行数，失败返回 -1
 */
int sbmp_db_del_server_port(void);

#endif /* SBMP_DB_H */
