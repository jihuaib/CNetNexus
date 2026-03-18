/**
 * @file   log.c
 * @brief  统一日志框架实现：按模块分文件，线程名替代 TID
 * @author jhb
 * @date   2026/02/18
 */

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/** 日志输出缓冲区大小 */
#define LOG_BUF_SIZE 2048

/** 最大支持注册的模块数量 */
#define LOG_MAX_MODULES 32

/** 全局日志级别，默认 DEBUG */
log_level_t g_log_level = LOG_LEVEL_DEBUG;

/** 当前线程的模块标签（线程局部，初始为 "unknown"） */
_Thread_local const char *g_log_tag = "unknown";

/* ============================================================
 * 模块日志文件注册表
 * ============================================================ */

typedef struct
{
    const char *tag; /**< 模块名（指向静态/堆字符串，生命周期须长于进程） */
    int fd;          /**< 文件描述符 */
} log_file_entry_t;

static log_file_entry_t g_log_files[LOG_MAX_MODULES];
static int g_log_file_count = 0;
static pthread_rwlock_t g_log_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/**
 * @brief 在注册表中查找 tag 对应的文件描述符（读锁）
 * @return fd（>=0）或 -1（未注册）
 */
static int find_module_fd(const char *tag)
{
    pthread_rwlock_rdlock(&g_log_rwlock);
    int fd = -1;
    for (int i = 0; i < g_log_file_count; i++)
    {
        if (strcmp(g_log_files[i].tag, tag) == 0)
        {
            fd = g_log_files[i].fd;
            break;
        }
    }
    pthread_rwlock_unlock(&g_log_rwlock);
    return fd;
}

int log_register_module(const char *tag, const char *path)
{
    if (!tag || !path)
    {
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        return -1;
    }

    pthread_rwlock_wrlock(&g_log_rwlock);

    /* 若已注册同 tag，更新 fd */
    for (int i = 0; i < g_log_file_count; i++)
    {
        if (strcmp(g_log_files[i].tag, tag) == 0)
        {
            close(g_log_files[i].fd);
            g_log_files[i].fd = fd;
            pthread_rwlock_unlock(&g_log_rwlock);
            return 0;
        }
    }

    /* 新增注册 */
    if (g_log_file_count < LOG_MAX_MODULES)
    {
        g_log_files[g_log_file_count].tag = tag;
        g_log_files[g_log_file_count].fd = fd;
        g_log_file_count++;
    }
    else
    {
        close(fd);
    }

    pthread_rwlock_unlock(&g_log_rwlock);
    return 0;
}

int log_register_module_auto(const char *tag)
{
    const char *work_dir = getenv("NN_WORK_DIR");
    if (!work_dir)
    {
        return 0; /* 未设置 work_dir，仅输出到 stderr */
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/log/%s.log", work_dir, tag);
    return log_register_module(tag, path);
}

void log_open_file(const char *path)
{
    if (!path)
    {
        return;
    }
    log_register_module("main", path);
}

/* ============================================================
 * 基础 API
 * ============================================================ */

void log_set_tag(const char *tag)
{
    g_log_tag = tag ? tag : "unknown";
}

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

/* ============================================================
 * 日志输出
 * ============================================================ */

/** 日志级别名称映射 */
static const char *level_names[] = {
    [LOG_LEVEL_DEBUG] = "DEBUG",
    [LOG_LEVEL_INFO] = "INFO ",
    [LOG_LEVEL_WARN] = "WARN ",
    [LOG_LEVEL_ERROR] = "ERROR",
};

/**
 * @brief 获取文件基名（去掉路径前缀）
 */
static const char *get_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/**
 * @brief 填写日志行头部（时间戳 + tag + 位置 + 线程名），返回已写入字节数
 */
static int write_header(char *buf, int buf_size, log_level_t level, const char *tag, const char *file, int line,
                        const char *func)
{
    /* 时间戳 HH:MM:SS.mmm */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    /* 文件名:行号，固定宽度保证后续列对齐 */
    char loc[32];
    snprintf(loc, sizeof(loc), "%s:%d", get_basename(file), line);

    /* 线程名（最长 15 字符，pthread 限制） */
    char thr_name[16] = "?";
    pthread_getname_np(pthread_self(), thr_name, sizeof(thr_name));

    return snprintf(buf, buf_size, "[%s][%-6s] %02d:%02d:%02d.%03d %-25s %s thr=%-15s | ", level_names[level], tag,
                    tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), loc, func, thr_name);
}

static void write_best_effort(int fd, const char *buf, size_t len)
{
    if (fd < 0 || !buf || len == 0)
    {
        return;
    }

    size_t offset = 0;
    while (offset < len)
    {
        ssize_t n = write(fd, buf + offset, len - offset);
        if (n > 0)
        {
            offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        break;
    }
}

/**
 * @brief 将 buf 写入 stderr 及模块专属日志文件
 */
static void flush_log(const char *tag, const char *buf, int len)
{
    if (!buf || len <= 0)
    {
        return;
    }

    write_best_effort(STDERR_FILENO, buf, (size_t)len);
    int fd = find_module_fd(tag);
    if (fd >= 0)
    {
        write_best_effort(fd, buf, (size_t)len);
    }
}

void log_output(log_level_t level, const char *tag, const char *file, int line, const char *func, const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];
    int offset = 0;

    offset += write_header(buf + offset, LOG_BUF_SIZE - offset, level, tag, file, line, func);

    va_list ap;
    va_start(ap, fmt);
    offset += vsnprintf(buf + offset, LOG_BUF_SIZE - offset, fmt, ap);
    va_end(ap);

    /* 换行（\r\n 兼容串口终端） */
    if (offset < LOG_BUF_SIZE - 2)
    {
        buf[offset++] = '\r';
        buf[offset++] = '\n';
    }

    flush_log(tag, buf, offset);
}

void log_output_perror(log_level_t level, const char *tag, const char *file, int line, const char *func,
                       const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];
    int offset = 0;

    /* 保存 errno，避免后续调用覆盖 */
    int saved_errno = errno;

    offset += write_header(buf + offset, LOG_BUF_SIZE - offset, level, tag, file, line, func);

    va_list ap;
    va_start(ap, fmt);
    offset += vsnprintf(buf + offset, LOG_BUF_SIZE - offset, fmt, ap);
    va_end(ap);

    /* 追加 errno 描述（GNU strerror_r 返回实际字符串指针） */
    char errbuf[256];
    const char *errstr = strerror_r(saved_errno, errbuf, sizeof(errbuf));
    offset += snprintf(buf + offset, LOG_BUF_SIZE - offset, ": %s", errstr);

    if (offset < LOG_BUF_SIZE - 2)
    {
        buf[offset++] = '\r';
        buf[offset++] = '\n';
    }

    flush_log(tag, buf, offset);
}
