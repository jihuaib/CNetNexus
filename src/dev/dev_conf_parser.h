/**
 * @file   dev_conf_parser.h
 * @brief  模块配置文件 (.conf) 解析器头文件
 * @author jhb
 * @date   2026/02/14
 */
#ifndef DEV_CONF_PARSER_H
#define DEV_CONF_PARSER_H

#include <stdint.h>

/** .conf 文件中 db 名称最大长度 */
#define DEV_CONF_DB_NAME_MAX 64

/** 模块配置结构体（从 module.conf 解析） */
typedef struct dev_module_conf
{
    uint32_t module_id;                 /**< module-id 字段 */
    char name[16];                      /**< name 字段 */
    char db_name[DEV_CONF_DB_NAME_MAX]; /**< db 字段（可选，为空表示无数据库） */
    char entry_func[64];               /**< entry 字段：入口函数名 */
    char apiso_name[64];                  /**< so 字段：共享库文件名 */
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
