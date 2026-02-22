/**
 * @file   log.h
 * @brief  统一日志框架公共头文件
 * @author jhb
 * @date   2026/02/18
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>

/**
 * @brief 日志级别枚举
 */
typedef enum
{
    LOG_LEVEL_DEBUG = 0, /**< 调试信息 */
    LOG_LEVEL_INFO = 1,  /**< 常规信息 */
    LOG_LEVEL_WARN = 2,  /**< 警告信息 */
    LOG_LEVEL_ERROR = 3, /**< 错误信息 */
} log_level_t;

/** 全局日志级别（运行时可配置） */
extern log_level_t g_log_level;

/**
 * @brief 初始化日志系统
 * @param level 初始日志级别
 */
void log_init(log_level_t level);

/**
 * @brief 设置日志级别
 * @param level 日志级别
 */
void log_set_level(log_level_t level);

/**
 * @brief 获取当前日志级别
 * @return 当前日志级别
 */
log_level_t log_get_level(void);

/**
 * @brief 通过名称字符串设置日志级别
 * @param name 级别名称（"debug"/"info"/"warn"/"error"，不区分大小写）
 * @return 0 成功，-1 无效名称
 */
int log_set_level_by_name(const char *name);

/**
 * @brief 日志输出内部函数（不要直接调用，使用 LOG_xxx 宏）
 * @param level 日志级别
 * @param tag 模块标签
 * @param file 源文件名
 * @param line 行号
 * @param func 函数名
 * @param fmt 格式化字符串
 */
void log_output(log_level_t level, const char *tag, const char *file, int line, const char *func, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

/**
 * @brief 日志输出宏（带 errno 描述，替代 perror）
 * @param level 日志级别
 * @param tag 模块标签
 * @param file 源文件名
 * @param line 行号
 * @param func 函数名
 * @param fmt 格式化字符串
 */
void log_output_perror(log_level_t level, const char *tag, const char *file, int line, const char *func,
                       const char *fmt, ...) __attribute__((format(printf, 6, 7)));

#ifndef LOG_TAG
#    define LOG_TAG "unknown"
#endif

/** 调试日志 */
#define LOG_DEBUG(fmt, ...)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        if (g_log_level <= LOG_LEVEL_DEBUG)                                                                            \
            log_output(LOG_LEVEL_DEBUG, LOG_TAG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);                    \
    } while (0)

/** 信息日志 */
#define LOG_INFO(fmt, ...)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (g_log_level <= LOG_LEVEL_INFO)                                                                             \
            log_output(LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);                     \
    } while (0)

/** 警告日志 */
#define LOG_WARN(fmt, ...)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (g_log_level <= LOG_LEVEL_WARN)                                                                             \
            log_output(LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);                     \
    } while (0)

/** 错误日志 */
#define LOG_ERROR(fmt, ...)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        if (g_log_level <= LOG_LEVEL_ERROR)                                                                            \
            log_output(LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);                    \
    } while (0)

/** 替代 perror()，输出 errno 描述 */
#define LOG_PERROR(fmt, ...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        log_output_perror(LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);                 \
    } while (0)

#endif // LOG_H
