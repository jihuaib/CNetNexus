/**
 * @file   path_utils.h
 * @brief  路径工具函数，提供可执行文件目录获取和 XML 配置路径解析
 * @author jhb
 * @date   2026/01/22
 */

#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include <stddef.h>

/**
 * @brief 获取可执行文件所在目录
 * @param buf 存储目录路径的缓冲区
 * @param size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_exe_dir(char *buf, size_t size);

/**
 * @brief 解析模块的 XML 配置文件路径
 * @param module_name 模块名称（如 "bgp"、"dev"、"cfg"）
 * @param buf 存储解析后路径的缓冲区
 * @param size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 *
 * 按以下优先级解析 XML 路径：
 * 1. 环境变量 NN_WORK_DIR（取 resources 子目录）
 * 2. 相对于可执行文件目录（开发构建）
 * 3. 系统安装路径（生产环境）
 */
int resolve_xml_path(const char *module_name, char *buf, size_t size);

/**
 * @brief 读取文本文件第一行并去掉换行
 * @param path 文件路径
 * @param out 输出缓冲
 * @param out_size 输出缓冲大小
 * @return 成功返回 0，失败返回 -1
 */
int file_read_first_line(const char *path, char *out, size_t out_size);

/**
 * @brief 解析 NetNexus VERSION 文件路径
 *
 * 优先级：
 * 1. $NN_WORK_DIR/VERSION
 * 2. 相对于可执行文件目录的 ../../VERSION
 * 3. 当前目录 VERSION
 *
 * @param path 输出路径缓冲
 * @param path_size 缓冲大小
 * @return 成功返回 0，失败返回 -1
 */
int resolve_version_file(char *path, size_t path_size);

/**
 * @brief 读取当前 NetNexus 版本字符串
 * @param version 输出缓冲
 * @param version_size 缓冲大小
 * @return 成功返回 0，失败返回 -1
 */
int read_current_version(char *version, size_t version_size);

#endif // PATH_UTILS_H
