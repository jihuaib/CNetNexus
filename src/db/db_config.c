/**
 * @file   db_config.c
 * @brief  running/startup 配置管理实现
 * @author jhb
 * @date   2026/05/31
 */
#include "db_config.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "db_main.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 路径辅助
// ============================================================================

/**
 * @brief 获取 data 目录路径（NN_WORK_DIR/data 或 ./data）
 */
static void db_config_data_dir(char *buf, size_t size)
{
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir != NULL)
    {
        snprintf(buf, size, "%s/data", work_dir);
    }
    else
    {
        snprintf(buf, size, "./data");
    }
}

/**
 * @brief 递归创建目录
 */
static int db_config_mkdir_p(const char *path)
{
    char tmp[512];
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
    {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
    {
        return -1;
    }
    return 0;
}

int db_config_running_path(char *path_buf, size_t buf_size)
{
    char data_dir[512];
    db_config_data_dir(data_dir, sizeof(data_dir));
    snprintf(path_buf, buf_size, "%s/running.db", data_dir);
    return ERRCODE_SUCCESS;
}

/**
 * @brief 命名快照路径：data/configs/<name>.db
 */
static void db_config_snapshot_path(const char *name, char *buf, size_t size)
{
    char data_dir[512];
    db_config_data_dir(data_dir, sizeof(data_dir));
    snprintf(buf, size, "%s/configs/%s.db", data_dir, name);
}

/**
 * @brief startup 指针文件路径：data/startup.cfg
 */
static void db_config_startup_ptr_path(char *buf, size_t size)
{
    char data_dir[512];
    db_config_data_dir(data_dir, sizeof(data_dir));
    snprintf(buf, size, "%s/startup.cfg", data_dir);
}

// ============================================================================
// 配置名校验
// ============================================================================

/**
 * @brief 校验配置名合法性：非空、长度限制、仅 [A-Za-z0-9_-]，防路径穿越
 */
static gboolean db_config_name_valid(const char *name)
{
    if (!name || name[0] == '\0')
    {
        return FALSE;
    }
    if (strlen(name) > DB_CONFIG_NAME_MAX)
    {
        return FALSE;
    }
    for (const char *p = name; *p; p++)
    {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
        {
            return FALSE;
        }
    }
    return TRUE;
}

// ============================================================================
// startup 指针读写
// ============================================================================

int db_config_get_startup(char *name_buf, size_t buf_size)
{
    name_buf[0] = '\0';

    char ptr_path[512];
    db_config_startup_ptr_path(ptr_path, sizeof(ptr_path));

    FILE *fp = fopen(ptr_path, "r");
    if (!fp)
    {
        return ERRCODE_SUCCESS; /* 无指针 = 出厂启动 */
    }

    if (fgets(name_buf, (int)buf_size, fp))
    {
        /* 去掉尾部空白/换行 */
        size_t n = strlen(name_buf);
        while (n > 0 && (name_buf[n - 1] == '\n' || name_buf[n - 1] == '\r' || name_buf[n - 1] == ' ' ||
                         name_buf[n - 1] == '\t'))
        {
            name_buf[--n] = '\0';
        }
    }
    fclose(fp);

    /* 防御：文件内容若被污染则当作无指针 */
    if (name_buf[0] != '\0' && !db_config_name_valid(name_buf))
    {
        LOG_WARN("DB-CONFIG: invalid startup pointer content, ignoring");
        name_buf[0] = '\0';
    }

    return ERRCODE_SUCCESS;
}

/**
 * @brief 原子写 startup 指针：写临时文件后 rename
 */
static int db_config_write_startup_ptr(const char *name)
{
    char data_dir[512];
    db_config_data_dir(data_dir, sizeof(data_dir));
    if (db_config_mkdir_p(data_dir) != 0)
    {
        LOG_ERROR("DB-CONFIG: failed to create data dir: %s", data_dir);
        return ERRCODE_FAIL;
    }

    char ptr_path[512];
    char tmp_path[600];
    db_config_startup_ptr_path(ptr_path, sizeof(ptr_path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", ptr_path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp)
    {
        LOG_ERROR("DB-CONFIG: failed to open %s: %s", tmp_path, strerror(errno));
        return ERRCODE_FAIL;
    }
    fprintf(fp, "%s\n", name);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (rename(tmp_path, ptr_path) != 0)
    {
        LOG_ERROR("DB-CONFIG: failed to rename %s -> %s: %s", tmp_path, ptr_path, strerror(errno));
        unlink(tmp_path);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

// ============================================================================
// SQLite 整库拷贝（Online Backup API，WAL 安全）
// ============================================================================

/**
 * @brief 用 backup API 把 src_path 整库拷贝到 dst_path
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
static int db_config_backup_db(const char *src_path, const char *dst_path)
{
    sqlite3 *src = NULL;
    sqlite3 *dst = NULL;
    int ret = ERRCODE_FAIL;

    if (sqlite3_open_v2(src_path, &src, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
        LOG_ERROR("DB-CONFIG: open src %s failed: %s", src_path, src ? sqlite3_errmsg(src) : "?");
        goto out;
    }

    if (sqlite3_open(dst_path, &dst) != SQLITE_OK)
    {
        LOG_ERROR("DB-CONFIG: open dst %s failed: %s", dst_path, dst ? sqlite3_errmsg(dst) : "?");
        goto out;
    }

    sqlite3_backup *bk = sqlite3_backup_init(dst, "main", src, "main");
    if (!bk)
    {
        LOG_ERROR("DB-CONFIG: backup_init failed: %s", sqlite3_errmsg(dst));
        goto out;
    }

    int rc = sqlite3_backup_step(bk, -1); /* -1 = 一次性拷贝全部页 */
    sqlite3_backup_finish(bk);

    if (rc != SQLITE_DONE)
    {
        LOG_ERROR("DB-CONFIG: backup_step failed: %s", sqlite3_errmsg(dst));
        goto out;
    }

    ret = ERRCODE_SUCCESS;

out:
    if (src)
    {
        sqlite3_close(src);
    }
    if (dst)
    {
        sqlite3_close(dst);
    }
    return ret;
}

// ============================================================================
// 开机预处理
// ============================================================================

/**
 * @brief 删除运行库及其 WAL/SHM 旁文件
 */
static void db_config_remove_running(const char *running_path)
{
    char aux[600];
    unlink(running_path);
    snprintf(aux, sizeof(aux), "%s-wal", running_path);
    unlink(aux);
    snprintf(aux, sizeof(aux), "%s-shm", running_path);
    unlink(aux);
    snprintf(aux, sizeof(aux), "%s-journal", running_path);
    unlink(aux);
}

int db_config_boot_prepare(void)
{
    char data_dir[512];
    db_config_data_dir(data_dir, sizeof(data_dir));
    if (db_config_mkdir_p(data_dir) != 0)
    {
        LOG_ERROR("DB-CONFIG: failed to create data dir: %s", data_dir);
        /* 继续，后续 open 仍会再尝试建目录 */
    }

    char running_path[512];
    db_config_running_path(running_path, sizeof(running_path));

    /* 0. 热重启（process start/reboot db，整机未掉电、业务模块仍在线持有内存配置）：
     * 磁盘上的 running.db 即当前运行配置，必须原样保留——既不清残留、也不按 startup
     * 指针覆盖，否则会把仍然有效的运行配置抹掉，造成内存/OS 与 DB 静默偏移。
     * NN_WARM_RESTART 由 DEV 在 respawn 子进程时通过环境变量传入（见 dev_module.c）。 */
    if (getenv("NN_WARM_RESTART") != NULL)
    {
        LOG_INFO("DB-CONFIG: warm restart, preserving existing running db (%s)", running_path);
        return ERRCODE_SUCCESS;
    }

    /* 1. 清除上次运行残留：运行库是临时库，整机掉电/冷启动即丢 */
    db_config_remove_running(running_path);

    /* 2. 读 startup 指针 */
    char name[DB_CONFIG_NAME_MAX + 1];
    db_config_get_startup(name, sizeof(name));

    if (name[0] == '\0')
    {
        LOG_INFO("DB-CONFIG: no startup configuration, booting with empty running db");
        return ERRCODE_SUCCESS;
    }

    /* 3. startup 指向的快照存在则恢复到运行库 */
    char snap_path[600];
    db_config_snapshot_path(name, snap_path, sizeof(snap_path));

    if (access(snap_path, R_OK) != 0)
    {
        LOG_WARN("DB-CONFIG: startup config '%s' not found (%s), booting empty", name, snap_path);
        return ERRCODE_SUCCESS;
    }

    if (db_config_backup_db(snap_path, running_path) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("DB-CONFIG: restore startup '%s' failed, booting empty", name);
        /* 恢复失败：清掉可能写了一半的运行库，退化为空库 */
        db_config_remove_running(running_path);
        return ERRCODE_SUCCESS;
    }

    LOG_INFO("DB-CONFIG: restored startup configuration '%s' into running db", name);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// save configuration
// ============================================================================

/**
 * @brief 设置错误描述（err 非空时分配字符串）
 */
static void db_config_set_err(char **err, const char *msg)
{
    if (err)
    {
        *err = g_strdup(msg);
    }
}

int db_config_save(const char *name, char **err)
{
    if (err)
    {
        *err = NULL;
    }

    /* 省略名称时回退到当前 startup 名或默认名 */
    char resolved[DB_CONFIG_NAME_MAX + 1];
    if (name && name[0] != '\0')
    {
        snprintf(resolved, sizeof(resolved), "%s", name);
    }
    else
    {
        db_config_get_startup(resolved, sizeof(resolved));
        if (resolved[0] == '\0')
        {
            snprintf(resolved, sizeof(resolved), "%s", DB_CONFIG_DEFAULT_NAME);
        }
    }

    if (!db_config_name_valid(resolved))
    {
        db_config_set_err(err, "Invalid configuration name (allowed: A-Z a-z 0-9 _ - , max 63)");
        return ERRCODE_FAIL;
    }

    db_connection_t *conn = g_db_local ? g_db_local->main_conn : NULL;
    if (!conn || !conn->handle)
    {
        db_config_set_err(err, "Running database not open");
        return ERRCODE_DB_NOT_OPEN;
    }

    /* 确保 configs 目录存在 */
    char data_dir[512];
    char configs_dir[600];
    db_config_data_dir(data_dir, sizeof(data_dir));
    snprintf(configs_dir, sizeof(configs_dir), "%s/configs", data_dir);
    if (db_config_mkdir_p(configs_dir) != 0)
    {
        db_config_set_err(err, "Failed to create configs directory");
        return ERRCODE_FAIL;
    }

    char dst_path[700];
    char tmp_path[760];
    db_config_snapshot_path(resolved, dst_path, sizeof(dst_path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dst_path);

    /* 清掉可能残留的临时文件 */
    db_config_remove_running(tmp_path); /* 复用：删 tmp 及其旁文件 */

    /* 用 backup API 把运行库（含未 checkpoint 的 WAL）快照到临时文件，
     * src 直接读 main_conn 句柄以保证拿到内存中最新一致状态 */
    int ret = ERRCODE_FAIL;
    sqlite3 *dst = NULL;

    g_mutex_lock(&conn->db_mutex);

    if (sqlite3_open(tmp_path, &dst) != SQLITE_OK)
    {
        LOG_ERROR("DB-CONFIG: open tmp %s failed: %s", tmp_path, dst ? sqlite3_errmsg(dst) : "?");
        g_mutex_unlock(&conn->db_mutex);
        db_config_set_err(err, "Failed to create snapshot file");
        if (dst)
        {
            sqlite3_close(dst);
        }
        return ERRCODE_FAIL;
    }

    sqlite3_backup *bk = sqlite3_backup_init(dst, "main", conn->handle, "main");
    if (!bk)
    {
        LOG_ERROR("DB-CONFIG: backup_init failed: %s", sqlite3_errmsg(dst));
        g_mutex_unlock(&conn->db_mutex);
        db_config_set_err(err, "Failed to start snapshot");
        sqlite3_close(dst);
        unlink(tmp_path);
        return ERRCODE_FAIL;
    }

    int rc = sqlite3_backup_step(bk, -1);
    sqlite3_backup_finish(bk);
    sqlite3_close(dst);

    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_DONE)
    {
        LOG_ERROR("DB-CONFIG: backup_step failed rc=%d", rc);
        db_config_set_err(err, "Snapshot copy failed");
        unlink(tmp_path);
        return ERRCODE_FAIL;
    }

    /* 原子落盘 */
    if (rename(tmp_path, dst_path) != 0)
    {
        LOG_ERROR("DB-CONFIG: rename %s -> %s failed: %s", tmp_path, dst_path, strerror(errno));
        db_config_set_err(err, "Failed to commit snapshot");
        unlink(tmp_path);
        return ERRCODE_FAIL;
    }

    LOG_INFO("DB-CONFIG: saved running configuration as '%s'", resolved);
    ret = ERRCODE_SUCCESS;
    return ret;
}

// ============================================================================
// startup configuration
// ============================================================================

int db_config_set_startup(const char *name, char **err)
{
    if (err)
    {
        *err = NULL;
    }

    if (!db_config_name_valid(name))
    {
        db_config_set_err(err, "Invalid configuration name (allowed: A-Z a-z 0-9 _ - , max 63)");
        return ERRCODE_FAIL;
    }

    char snap_path[700];
    db_config_snapshot_path(name, snap_path, sizeof(snap_path));
    if (access(snap_path, R_OK) != 0)
    {
        db_config_set_err(err, "Configuration not found; use 'save configuration <name>' first");
        return ERRCODE_FAIL;
    }

    if (db_config_write_startup_ptr(name) != ERRCODE_SUCCESS)
    {
        db_config_set_err(err, "Failed to write startup pointer");
        return ERRCODE_FAIL;
    }

    LOG_INFO("DB-CONFIG: startup configuration set to '%s'", name);
    return ERRCODE_SUCCESS;
}
