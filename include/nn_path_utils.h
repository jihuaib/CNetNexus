/**
 * @file   nn_path_utils.h
 * @brief  路径工具函数，提供可执行文件目录获取和 XML 配置路径解析
 * @author jhb
 * @date   2026/01/22
 */

#ifndef NN_PATH_UTILS_H
#define NN_PATH_UTILS_H

#include <stddef.h>

/**
 * @brief 获取可执行文件所在目录
 * @param buf 存储目录路径的缓冲区
 * @param size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int nn_get_exe_dir(char *buf, size_t size);

/**
 * @brief 解析模块的 XML 配置文件路径
 * @param module_name 模块名称（如 "bgp"、"dev"、"cfg"）
 * @param buf 存储解析后路径的缓冲区
 * @param size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 *
 * 按以下优先级解析 XML 路径：
 * 1. 环境变量 NN_RESOURCES_DIR（如已设置）
 * 2. 相对于可执行文件目录（开发构建）
 * 3. 系统安装路径（生产环境）
 */
int nn_resolve_xml_path(const char *module_name, char *buf, size_t size);

#endif // NN_PATH_UTILS_H
