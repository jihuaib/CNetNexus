/**
 * @file   dev_conf_parser.h
 * @brief  模块配置文件 (.conf) 解析器头文件
 * @author jhb
 * @date   2026/02/14
 */
#ifndef DEV_CONF_PARSER_H
#define DEV_CONF_PARSER_H

#include <stdint.h>

/** 模块配置结构体（从 module.conf 解析） */
typedef struct dev_module_conf
{
    uint32_t module_id;    /**< module-id 字段 */
    char name[16];         /**< name 字段 */
    char exe_name[64];     /**< exe 字段：独立进程可执行文件名 */
    uint16_t port;         /**< port 字段：IPC 监听端口，dev连接使用 */
    uint8_t on_demand;     /**< on-demand 字段：1=按需启动（首次被 CFG 触发时由 DEV fork），0=常驻 */
    char revive_table[64]; /**< revive-table 字段：on-demand 模块的"配置存在标识表"；
                                DEV 在 boot 时扫到此表非空即自动 fork，实现 DB 配置自动恢复 */
} dev_module_conf_t;

/**
 * @brief 解析 module.conf 文件
 * @param path 配置文件路径
 * @param conf 输出配置结构体
 * @return 成功返回 0，失败返回 -1
 */
int dev_conf_parse(const char *path, dev_module_conf_t *conf);

/**
 * @brief 解析指定模块名称的 module.conf（自动发现路径）
 * @param module_name 模块名称（如 "bgp"）
 * @param conf 输出配置结构体
 * @return 成功返回 0，失败返回 -1
 */
int dev_conf_resolve_and_parse(const char *module_name, dev_module_conf_t *conf);

#endif // DEV_CONF_PARSER_H
