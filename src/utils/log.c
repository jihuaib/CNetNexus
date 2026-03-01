/**
 * @file   log.c
 * @brief  统一日志框架实现
 * @author jhb
 * @date   2026/02/18
 */

#define _GNU_SOURCE
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/** 日志输出缓冲区大小 */
#define LOG_BUF_SIZE 2048

/** 全局日志级别，默认 DEBUG */
log_level_t g_log_level = LOG_LEVEL_DEBUG;

/** 日志文件描述符（-1 表示未启用文件输出） */
static int g_log_fd = -1;

void log_open_file(const char *path)
{
    if (g_log_fd >= 0)
    {
        close(g_log_fd);
        g_log_fd = -1;
    }
    if (!path)
    {
        return;
    }
    g_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}

/** 当前线程的模块标签（线程局部，初始为 "unknown"） */
_Thread_local const char *g_log_tag = "unknown";

void log_set_tag(const char *tag)
{
    g_log_tag = tag ? tag : "unknown";
}

/** 日志级别名称映射 */
static const char *level_names[] = {
    [LOG_LEVEL_DEBUG] = "DEBUG",
    [LOG_LEVEL_INFO] = "INFO ",
    [LOG_LEVEL_WARN] = "WARN ",
    [LOG_LEVEL_ERROR] = "ERROR",
};

void log_init(log_level_t level)
{
    g_log_level = level;
}

void log_set_level(log_level_t level)
{
    g_log_level = level;
}

log_level_t log_get_level(void)
{
    return g_log_level;
}

int log_set_level_by_name(const char *name)
{
    if (strcasecmp(name, "debug") == 0)
    {
        g_log_level = LOG_LEVEL_DEBUG;
    }
    else if (strcasecmp(name, "info") == 0)
    {
        g_log_level = LOG_LEVEL_INFO;
    }
    else if (strcasecmp(name, "warn") == 0)
    {
        g_log_level = LOG_LEVEL_WARN;
    }
    else if (strcasecmp(name, "error") == 0)
    {
        g_log_level = LOG_LEVEL_ERROR;
    }
    else
    {
        return -1;
    }
    return 0;
}

/**
 * @brief 获取文件基名（去掉路径前缀）
 */
static const char *get_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void log_output(log_level_t level, const char *tag, const char *file, int line, const char *func, const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];
    int offset = 0;

    /* 时间戳 HH:MM:SS.mmm */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    /* 将文件名:行号组合为固定宽度字段，保证后续列对齐 */
    char loc[32];
    snprintf(loc, sizeof(loc), "%s:%d", get_basename(file), line);

    offset += snprintf(buf + offset, LOG_BUF_SIZE - offset, "[%s][%-6s] %02d:%02d:%02d.%03d %-25s %s tid=%ld | ",
                       level_names[level], tag, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), loc, func,
                       syscall(SYS_gettid));

    /* 用户消息 */
    va_list ap;
    va_start(ap, fmt);
    offset += vsnprintf(buf + offset, LOG_BUF_SIZE - offset, fmt, ap);
    va_end(ap);

    /* 换行 */
    if (offset < LOG_BUF_SIZE - 1)
    {
        buf[offset++] = '\n';
    }

    /* 写入 stderr（串口/终端） */
    write(STDERR_FILENO, buf, offset);
    /* 同时写入日志文件（若已打开） */
    if (g_log_fd >= 0)
    {
        write(g_log_fd, buf, offset);
    }
}

void log_output_perror(log_level_t level, const char *tag, const char *file, int line, const char *func,
                       const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];
    int offset = 0;

    /* 保存 errno */
    int saved_errno = errno;

    /* 时间戳 */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    /* 将文件名:行号组合为固定宽度字段，保证后续列对齐 */
    char loc[32];
    snprintf(loc, sizeof(loc), "%s:%d", get_basename(file), line);

    offset += snprintf(buf + offset, LOG_BUF_SIZE - offset, "[%s][%-6s] %02d:%02d:%02d.%03d %-25s %s tid=%ld | ",
                       level_names[level], tag, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), loc, func,
                       syscall(SYS_gettid));

    /* 用户消息 */
    va_list ap;
    va_start(ap, fmt);
    offset += vsnprintf(buf + offset, LOG_BUF_SIZE - offset, fmt, ap);
    va_end(ap);

    /* errno 描述 */
    char errbuf[256];
    strerror_r(saved_errno, errbuf, sizeof(errbuf));
    offset += snprintf(buf + offset, LOG_BUF_SIZE - offset, ": %s", errbuf);

    /* 换行 */
    if (offset < LOG_BUF_SIZE - 1)
    {
        buf[offset++] = '\n';
    }

    write(STDERR_FILENO, buf, offset);
    if (g_log_fd >= 0)
    {
        write(g_log_fd, buf, offset);
    }
}
