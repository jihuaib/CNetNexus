/**
 * @file   bgp_bmp_db.h
 * @brief  BGP BMP 上报功能数据库操作接口
 * @author jhb
 * @date   2026/03/29
 */
#ifndef BGP_BMP_DB_H
#define BGP_BMP_DB_H

#include <stdint.h>

/** BMP 实例配置表名 */
#define BGP_TABLE_BMP_INSTANCE "bgp_bmp_instance"
/** BMP 监控邻居表名 */
#define BGP_TABLE_BMP_MONITOR "bgp_bmp_monitor"

/**
 * @brief 获取 BMP 表定义数组（供 bgp_db_init 注册建表）
 * @param count 输出表数量
 * @return 表定义指针数组
 */
const void **bgp_bmp_db_get_tables(int *count);

/**
 * @brief 创建或进入 BMP 实例（幂等，已存在时不覆盖）
 * @param instance_name 实例名
 * @return 0 成功，-1 失败
 */
int bgp_bmp_db_set_instance(const char *instance_name);

/**
 * @brief 删除 BMP 实例及其关联的监控邻居配置
 * @param instance_name 实例名
 * @return 删除的总行数，-1 失败
 */
int bgp_bmp_db_del_instance(const char *instance_name);

/**
 * @brief 设置 BMP 实例的 collector 地址和端口
 * @param instance_name 实例名
 * @param ip            采集器 IP 地址
 * @param port          采集器端口
 * @return 0 成功，-1 失败
 */
int bgp_bmp_db_set_collector(const char *instance_name, const char *ip, uint16_t port);

/**
 * @brief 清除 BMP 实例的 collector 配置
 * @param instance_name 实例名
 * @return 0 成功，-1 失败
 */
int bgp_bmp_db_del_collector(const char *instance_name);

/**
 * @brief 设置 BMP 实例的 Stats Report 间隔
 * @param instance_name 实例名
 * @param interval      间隔（秒），0 表示禁用
 * @return 0 成功，-1 失败
 */
int bgp_bmp_db_set_stats_interval(const char *instance_name, uint16_t interval);

/**
 * @brief 重置 BMP 实例的 Stats Report 间隔为默认值（0=禁用）
 * @param instance_name 实例名
 * @return 0 成功，-1 失败
 */
int bgp_bmp_db_del_stats_interval(const char *instance_name);

/**
 * @brief 设置 BMP 实例的重连间隔
 * @param instance_name 实例名
 * @param interval      间隔（秒）
 * @return 0 成功，-1 失败
 */
int bgp_bmp_db_set_reconnect_interval(const char *instance_name, uint16_t interval);

/**
 * @brief 重置 BMP 实例的重连间隔为默认值（30 秒）
 * @param instance_name 实例名
 * @return 0 成功，-1 失败
 */
int bgp_bmp_db_del_reconnect_interval(const char *instance_name);

/**
 * @brief 设置监控全部邻居
 * @param instance_name 实例名
 * @return 0 成功，-1 失败
 */
int bgp_bmp_db_set_monitor_all(const char *instance_name);

/**
 * @brief 添加指定邻居到监控列表（同时将 monitor_all 设为 0）
 * @param instance_name 实例名
 * @param neighbor_ip   邻居 IP
 * @return 0 成功，-1 失败
 */
int bgp_bmp_db_add_monitor_peer(const char *instance_name, const char *neighbor_ip);

/**
 * @brief 从监控列表移除指定邻居
 * @param instance_name 实例名
 * @param neighbor_ip   邻居 IP
 * @return 删除的行数，-1 失败
 */
int bgp_bmp_db_del_monitor_peer(const char *instance_name, const char *neighbor_ip);

/**
 * @brief 从 DB 恢复所有 BMP 实例到运行态
 *
 * 读取 bgp_bmp_instance 和 bgp_bmp_monitor 表，
 * 通过 bgp_bmp_dispatch_apply() 逐一恢复到内存。
 */
void bgp_bmp_db_restore(void);

#endif /* BGP_BMP_DB_H */
