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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/** 日志输出缓冲区大小 */
#define LOG_BUF_SIZE 2048

/** 最大支持注册的模块数量 */
#define LOG_MAX_MODULES 32

/** 默认单文件最大字节数：10 MB */
#define LOG_DEFAULT_MAX_SIZE ((size_t)10 * 1024 * 1024)

/** 默认备份文件数量 */
#define LOG_DEFAULT_MAX_BACKUPS 5

/** 全局轮转配置（0 表示禁用） */
static size_t g_log_max_size = LOG_DEFAULT_MAX_SIZE;
static int g_log_max_backups = LOG_DEFAULT_MAX_BACKUPS;

/** 全局日志级别：Release 默认 WARN，Debug 默认 DEBUG（DEV 启动后会从 DB 覆盖） */
#ifdef NDEBUG
log_level_t g_log_level = LOG_LEVEL_WARN;
#else
log_level_t g_log_level = LOG_LEVEL_DEBUG;
#endif

/** 当前线程的模块标签（线程局部指针，始终指向 g_log_tag_buf） */
_Thread_local char g_log_tag_buf[32] = "unknown";
_Thread_local const char *g_log_tag = "unknown";

/* ============================================================
 * 模块日志文件注册表
 * ============================================================ */

typedef struct
{
    char *tag;                  /**< 模块名（注册表持有副本） */
    char *path;                 /**< 日志文件路径（注册表持有副本，用于轮转重命名） */
    int fd;                     /**< 文件描述符 */
    size_t cur_size;            /**< 当前文件大小（字节，由 rotate_mtx 保护） */
    pthread_mutex_t rotate_mtx; /**< 串行化对该文件的写入与轮转 */
} log_file_entry_t;

static log_file_entry_t g_log_files[LOG_MAX_MODULES];
static int g_log_file_count = 0;
static pthread_rwlock_t g_log_rwlock = PTHREAD_RWLOCK_INITIALIZER;

void log_set_rotation(size_t max_size_bytes, int max_backups)
{
    g_log_max_size = max_size_bytes;
    g_log_max_backups = max_backups < 0 ? 0 : max_backups;
}

/**
 * @brief 在注册表中查找 tag 对应的 entry（读锁）
 * @return entry 指针或 NULL；返回的指针在进程生命期内稳定（不缩容、不移动）
 */
static log_file_entry_t *find_module_entry(const char *tag)
{
    pthread_rwlock_rdlock(&g_log_rwlock);
    log_file_entry_t *entry = NULL;
    for (int i = 0; i < g_log_file_count; i++)
    {
        if (strcmp(g_log_files[i].tag, tag) == 0)
        {
            entry = &g_log_files[i];
            break;
        }
    }
    pthread_rwlock_unlock(&g_log_rwlock);
    return entry;
}

/**
 * @brief 获取已打开文件的当前大小，失败返回 0
 */
static size_t fd_current_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0)
    {
        return (size_t)st.st_size;
    }
    return 0;
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
    size_t init_size = fd_current_size(fd);

    pthread_rwlock_wrlock(&g_log_rwlock);

    /* 若已注册同 tag，更新 fd 与 path */
    for (int i = 0; i < g_log_file_count; i++)
    {
        if (strcmp(g_log_files[i].tag, tag) == 0)
        {
            pthread_mutex_lock(&g_log_files[i].rotate_mtx);
            close(g_log_files[i].fd);
            g_log_files[i].fd = fd;
            g_log_files[i].cur_size = init_size;
            char *new_path = strdup(path);
            if (new_path)
            {
                free(g_log_files[i].path);
                g_log_files[i].path = new_path;
            }
            pthread_mutex_unlock(&g_log_files[i].rotate_mtx);
            pthread_rwlock_unlock(&g_log_rwlock);
            return 0;
        }
    }

    /* 新增注册 */
    if (g_log_file_count < LOG_MAX_MODULES)
    {
        char *tag_copy = strdup(tag);
        char *path_copy = strdup(path);
        if (!tag_copy || !path_copy)
        {
            free(tag_copy);
            free(path_copy);
            close(fd);
            pthread_rwlock_unlock(&g_log_rwlock);
            return -1;
        }
        log_file_entry_t *e = &g_log_files[g_log_file_count];
        e->tag = tag_copy;
        e->path = path_copy;
        e->fd = fd;
        e->cur_size = init_size;
        pthread_mutex_init(&e->rotate_mtx, NULL);
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
    if (!tag || tag[0] == '\0')
    {
        snprintf(g_log_tag_buf, sizeof(g_log_tag_buf), "%s", "unknown");
        g_log_tag = g_log_tag_buf;
        return;
    }
    snprintf(g_log_tag_buf, sizeof(g_log_tag_buf), "%s", tag);
    g_log_tag = g_log_tag_buf;
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
 * @brief 执行日志文件轮转：xxx.log -> xxx.log.1 -> ... -> xxx.log.N（最旧丢弃）
 * @details 调用方需持有 entry->rotate_mtx
 */
static void rotate_locked(log_file_entry_t *entry)
{
    if (!entry->path || g_log_max_size == 0)
    {
        return;
    }

    int max_backups = g_log_max_backups;
    char old_name[600];
    char new_name[600];

    /* 删除最旧的备份 */
    if (max_backups > 0)
    {
        snprintf(old_name, sizeof(old_name), "%s.%d", entry->path, max_backups);
        unlink(old_name); /* 忽略错误（可能不存在） */
    }
    else
    {
        /* 不保留备份：直接清空 */
        unlink(entry->path);
    }

    /* xxx.log.{i} -> xxx.log.{i+1}，从大到小 */
    for (int i = max_backups - 1; i >= 1; i--)
    {
        snprintf(old_name, sizeof(old_name), "%s.%d", entry->path, i);
        snprintf(new_name, sizeof(new_name), "%s.%d", entry->path, i + 1);
        rename(old_name, new_name); /* 忽略错误 */
    }

    /* xxx.log -> xxx.log.1 */
    if (max_backups > 0)
    {
        snprintf(new_name, sizeof(new_name), "%s.1", entry->path);
        rename(entry->path, new_name);
    }

    /* 重新打开新文件 */
    int new_fd = open(entry->path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (new_fd >= 0)
    {
        close(entry->fd);
        entry->fd = new_fd;
        entry->cur_size = 0;
    }
    /* 打开失败时保留原 fd 继续使用，避免日志丢失 */
}

/**
 * @brief 将 buf 写入 stderr 及模块专属日志文件，按需轮转
 */
static void flush_log(const char *tag, const char *buf, int len)
{
    if (!buf || len <= 0)
    {
        return;
    }

    write_best_effort(STDERR_FILENO, buf, (size_t)len);

    log_file_entry_t *entry = find_module_entry(tag);
    if (!entry)
    {
        return;
    }

    pthread_mutex_lock(&entry->rotate_mtx);
    if (entry->fd >= 0)
    {
        if (g_log_max_size > 0 && entry->cur_size + (size_t)len > g_log_max_size)
        {
            rotate_locked(entry);
        }
        write_best_effort(entry->fd, buf, (size_t)len);
        entry->cur_size += (size_t)len;
    }
    pthread_mutex_unlock(&entry->rotate_mtx);
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
