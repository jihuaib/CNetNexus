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
#include "path_utils.h"

#define DB_CONFIG_CFG_MAX_BYTES (16U * 1024U * 1024U)

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
    char data_dir[480];
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
 * @brief 命名 BDR 文本路径：data/configs/<name>.cfg
 */
static void db_config_cfg_path(const char *name, char *buf, size_t size)
{
    char data_dir[512];
    db_config_data_dir(data_dir, sizeof(data_dir));
    snprintf(buf, size, "%s/configs/%s.cfg", data_dir, name);
}

/**
 * @brief 命名配置元数据路径：data/configs/<name>.meta
 */
static void db_config_meta_path(const char *name, char *buf, size_t size)
{
    char data_dir[512];
    db_config_data_dir(data_dir, sizeof(data_dir));
    snprintf(buf, size, "%s/configs/%s.meta", data_dir, name);
}

/**
 * @brief startup 指针文件路径：data/startup.cfg
 */
static void db_config_startup_ptr_path(char *buf, size_t size)
{
    char data_dir[480];
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

const char *db_config_startup_mode_name(db_config_startup_mode_t mode)
{
    switch (mode)
    {
        case DB_CONFIG_STARTUP_MODE_DB:
            return "db";
        case DB_CONFIG_STARTUP_MODE_CFG:
            return "cfg";
        case DB_CONFIG_STARTUP_MODE_NONE:
        default:
            return "none";
    }
}

static gboolean db_config_parse_startup_mode(const char *text, db_config_startup_mode_t *mode)
{
    if (!text || !mode)
    {
        return FALSE;
    }

    if (strcmp(text, "db") == 0)
    {
        *mode = DB_CONFIG_STARTUP_MODE_DB;
        return TRUE;
    }
    if (strcmp(text, "cfg") == 0)
    {
        *mode = DB_CONFIG_STARTUP_MODE_CFG;
        return TRUE;
    }

    return FALSE;
}

static gboolean db_config_startup_mode_valid(db_config_startup_mode_t mode)
{
    return mode == DB_CONFIG_STARTUP_MODE_DB || mode == DB_CONFIG_STARTUP_MODE_CFG;
}

// ============================================================================
// startup 指针读写
// ============================================================================

int db_config_get_startup(char *name_buf, size_t buf_size, db_config_startup_mode_t *mode)
{
    if (name_buf && buf_size > 0)
    {
        name_buf[0] = '\0';
    }
    if (mode)
    {
        *mode = DB_CONFIG_STARTUP_MODE_NONE;
    }

    char ptr_path[512];
    db_config_startup_ptr_path(ptr_path, sizeof(ptr_path));

    gchar *content = NULL;
    if (!g_file_get_contents(ptr_path, &content, NULL, NULL))
    {
        return ERRCODE_SUCCESS; /* 无指针 = 出厂启动 */
    }

    char mode_text[16] = "";
    char parsed_name[DB_CONFIG_NAME_MAX + 1] = "";
    char extra[2] = "";
    char *trimmed = g_strstrip(content);
    int fields = sscanf(trimmed, "%15s %63s %1s", mode_text, parsed_name, extra);

    db_config_startup_mode_t parsed_mode = DB_CONFIG_STARTUP_MODE_NONE;
    if (fields == 2 && db_config_parse_startup_mode(mode_text, &parsed_mode) && db_config_name_valid(parsed_name))
    {
        if (name_buf && buf_size > 0)
        {
            snprintf(name_buf, buf_size, "%s", parsed_name);
        }
        if (mode)
        {
            *mode = parsed_mode;
        }
    }
    else if (trimmed[0] != '\0')
    {
        LOG_WARN("DB-CONFIG: invalid startup pointer content, ignoring");
    }

    g_free(content);
    return ERRCODE_SUCCESS;
}

/**
 * @brief 原子写 startup 指针：写临时文件后 rename
 */
static int db_config_write_startup_ptr(const char *name, db_config_startup_mode_t mode)
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
    fprintf(fp, "%s %s\n", db_config_startup_mode_name(mode), name);
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
// 版本兼容判断
// ============================================================================

static gboolean db_config_parse_major_version(const char *version, int *major)
{
    if (!version || !major)
    {
        return FALSE;
    }

    while (*version == ' ' || *version == '\t')
    {
        version++;
    }
    if (*version < '0' || *version > '9')
    {
        return FALSE;
    }

    char *end = NULL;
    long parsed = strtol(version, &end, 10);
    if (end == version || parsed < 0 || parsed > 255)
    {
        return FALSE;
    }

    *major = (int)parsed;
    return TRUE;
}

static gboolean db_config_read_saved_version(const char *name, char *version, size_t version_size)
{
    if (version && version_size > 0)
    {
        version[0] = '\0';
    }

    char meta_path[700];
    db_config_meta_path(name, meta_path, sizeof(meta_path));

    gchar *content = NULL;
    if (!g_file_get_contents(meta_path, &content, NULL, NULL))
    {
        return FALSE;
    }

    gboolean found = FALSE;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (guint i = 0; lines && lines[i]; i++)
    {
        char *line = g_strstrip(lines[i]);
        if (g_str_has_prefix(line, "version="))
        {
            char *value = g_strstrip(line + strlen("version="));
            if (value[0] != '\0')
            {
                g_strlcpy(version, value, version_size);
                found = TRUE;
                break;
            }
        }
    }

    g_strfreev(lines);
    g_free(content);
    return found;
}

static gboolean db_config_startup_major_mismatch(const char *name, char *saved_version, size_t saved_size,
                                                 char *current_version, size_t current_size)
{
    if (saved_version && saved_size > 0)
    {
        saved_version[0] = '\0';
    }
    if (current_version && current_size > 0)
    {
        current_version[0] = '\0';
    }

    char saved[64] = "";
    char current[64] = "";
    if (!db_config_read_saved_version(name, saved, sizeof(saved)))
    {
        LOG_WARN("DB-CONFIG: startup config '%s' has no version metadata, allowing db restore", name);
        return FALSE;
    }
    if (read_current_version(current, sizeof(current)) != 0)
    {
        LOG_WARN("DB-CONFIG: current version unavailable, allowing db restore for startup config '%s'", name);
        return FALSE;
    }

    if (saved_version && saved_size > 0)
    {
        g_strlcpy(saved_version, saved, saved_size);
    }
    if (current_version && current_size > 0)
    {
        g_strlcpy(current_version, current, current_size);
    }

    int saved_major = 0;
    int current_major = 0;
    if (!db_config_parse_major_version(saved, &saved_major) || !db_config_parse_major_version(current, &current_major))
    {
        LOG_WARN("DB-CONFIG: version metadata not parseable (saved='%s', current='%s'), allowing db restore", saved,
                 current);
        return FALSE;
    }

    return saved_major != current_major;
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
    db_config_startup_mode_t mode = DB_CONFIG_STARTUP_MODE_NONE;
    db_config_get_startup(name, sizeof(name), &mode);

    if (name[0] == '\0')
    {
        LOG_INFO("DB-CONFIG: no startup configuration, booting with empty running db");
        return ERRCODE_SUCCESS;
    }
    if (mode == DB_CONFIG_STARTUP_MODE_CFG)
    {
        LOG_INFO("DB-CONFIG: startup configuration '%s' uses cfg mode, booting empty for CLI replay", name);
        return ERRCODE_SUCCESS;
    }

    char saved_version[64] = "";
    char current_version[64] = "";
    if (db_config_startup_major_mismatch(name, saved_version, sizeof(saved_version), current_version,
                                         sizeof(current_version)))
    {
        LOG_WARN("DB-CONFIG: startup db configuration '%s' saved by version '%s' cannot restore on current version "
                 "'%s' (major mismatch); booting empty for cfg replay",
                 name, saved_version, current_version);
        return ERRCODE_SUCCESS;
    }

    /* 3. startup/db 指向的快照存在则恢复到运行库 */
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

    LOG_INFO("DB-CONFIG: restored startup db configuration '%s' into running db", name);
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

static int db_config_write_text_tmp(const char *path, const char *text, char **err)
{
    FILE *fp = fopen(path, "w");
    if (!fp)
    {
        LOG_ERROR("DB-CONFIG: failed to open %s: %s", path, strerror(errno));
        db_config_set_err(err, "Failed to create cfg text file");
        return ERRCODE_FAIL;
    }

    const char *safe_text = text ? text : "";
    if (fputs(safe_text, fp) == EOF)
    {
        LOG_ERROR("DB-CONFIG: failed to write %s: %s", path, strerror(errno));
        db_config_set_err(err, "Failed to write cfg text file");
        fclose(fp);
        unlink(path);
        return ERRCODE_FAIL;
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    return ERRCODE_SUCCESS;
}

static int db_config_write_meta_tmp(const char *path, const char *cfg_text, char **err)
{
    char version[64] = "unknown";
    if (read_current_version(version, sizeof(version)) != 0)
    {
        snprintf(version, sizeof(version), "unknown");
    }

    GString *meta = g_string_new("");
    g_string_append_printf(meta, "version=%s\n", version);
    g_string_append(meta, "format=bdr-indent-v1\n");
    g_string_append(meta, "capture_complete=1\n");

    gchar *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cfg_text ? cfg_text : "", -1);
    if (!checksum)
    {
        g_string_free(meta, TRUE);
        db_config_set_err(err, "Failed to checksum cfg text");
        return ERRCODE_FAIL;
    }
    g_string_append_printf(meta, "cfg_sha256=%s\n", checksum);
    g_free(checksum);

    int ret = db_config_write_text_tmp(path, meta->str, err);
    g_string_free(meta, TRUE);
    return ret;
}

static void db_config_restore_backup(const char *dst, const char *bak, gboolean had_backup)
{
    if (had_backup)
    {
        unlink(dst);
        if (rename(bak, dst) != 0)
        {
            LOG_ERROR("DB-CONFIG: failed to restore backup %s -> %s: %s", bak, dst, strerror(errno));
        }
    }
    else
    {
        unlink(dst);
    }
}

static void db_config_restore_backups(const char **dst_paths, char bak_paths[][800], const gboolean *had_backup,
                                      size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        db_config_restore_backup(dst_paths[i], bak_paths[i], had_backup[i]);
    }
}

static int db_config_publish_files(const char **tmp_paths, const char **dst_paths, size_t count, char **err)
{
    char bak_paths[3][800];
    gboolean had_backup[3] = {FALSE, FALSE, FALSE};

    if (count > G_N_ELEMENTS(bak_paths))
    {
        db_config_set_err(err, "Too many configuration files to publish");
        return ERRCODE_FAIL;
    }

    size_t backed = 0;
    for (size_t i = 0; i < count; i++)
    {
        snprintf(bak_paths[i], sizeof(bak_paths[i]), "%s.bak", dst_paths[i]);
        unlink(bak_paths[i]);
        had_backup[i] = (access(dst_paths[i], F_OK) == 0);
        if (had_backup[i] && rename(dst_paths[i], bak_paths[i]) != 0)
        {
            LOG_ERROR("DB-CONFIG: backup %s -> %s failed: %s", dst_paths[i], bak_paths[i], strerror(errno));
            db_config_restore_backups(dst_paths, bak_paths, had_backup, backed);
            db_config_set_err(err, "Failed to prepare old configuration files");
            return ERRCODE_FAIL;
        }
        backed++;
    }

    size_t published = 0;
    for (size_t i = 0; i < count; i++)
    {
        if (rename(tmp_paths[i], dst_paths[i]) != 0)
        {
            LOG_ERROR("DB-CONFIG: rename %s -> %s failed: %s", tmp_paths[i], dst_paths[i], strerror(errno));
            for (size_t j = 0; j < published; j++)
            {
                unlink(dst_paths[j]);
            }
            db_config_restore_backups(dst_paths, bak_paths, had_backup, count);
            db_config_set_err(err, "Failed to commit configuration files");
            return ERRCODE_FAIL;
        }
        published++;
    }

    for (size_t i = 0; i < count; i++)
    {
        unlink(bak_paths[i]);
    }
    return ERRCODE_SUCCESS;
}

int db_config_save(const char *name, const char *cfg_text, char *saved_name, size_t saved_name_cap, char **err)
{
    if (err)
    {
        *err = NULL;
    }
    if (saved_name && saved_name_cap > 0)
    {
        saved_name[0] = '\0';
    }

    if (!cfg_text)
    {
        db_config_set_err(err, "BDR cfg text is not available");
        return ERRCODE_FAIL;
    }

    /* 省略名称时回退到当前 startup 名或默认名 */
    char resolved[DB_CONFIG_NAME_MAX + 1];
    if (name && name[0] != '\0')
    {
        snprintf(resolved, sizeof(resolved), "%s", name);
    }
    else
    {
        db_config_get_startup(resolved, sizeof(resolved), NULL);
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
    if (saved_name && saved_name_cap > 0)
    {
        snprintf(saved_name, saved_name_cap, "%s", resolved);
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
    char cfg_path[700];
    char cfg_tmp_path[760];
    char meta_path[700];
    char meta_tmp_path[760];
    db_config_snapshot_path(resolved, dst_path, sizeof(dst_path));
    db_config_cfg_path(resolved, cfg_path, sizeof(cfg_path));
    db_config_meta_path(resolved, meta_path, sizeof(meta_path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dst_path);
    snprintf(cfg_tmp_path, sizeof(cfg_tmp_path), "%s.tmp", cfg_path);
    snprintf(meta_tmp_path, sizeof(meta_tmp_path), "%s.tmp", meta_path);

    /* 清掉可能残留的临时文件 */
    db_config_remove_running(tmp_path); /* 复用：删 tmp 及其旁文件 */
    unlink(cfg_tmp_path);
    unlink(meta_tmp_path);

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

    if (db_config_write_text_tmp(cfg_tmp_path, cfg_text, err) != ERRCODE_SUCCESS)
    {
        db_config_remove_running(tmp_path);
        return ERRCODE_FAIL;
    }

    if (db_config_write_meta_tmp(meta_tmp_path, cfg_text, err) != ERRCODE_SUCCESS)
    {
        db_config_remove_running(tmp_path);
        unlink(cfg_tmp_path);
        return ERRCODE_FAIL;
    }

    const char *tmp_paths[] = {tmp_path, cfg_tmp_path, meta_tmp_path};
    const char *dst_paths[] = {dst_path, cfg_path, meta_path};
    if (db_config_publish_files(tmp_paths, dst_paths, G_N_ELEMENTS(tmp_paths), err) != ERRCODE_SUCCESS)
    {
        db_config_remove_running(tmp_path);
        unlink(cfg_tmp_path);
        unlink(meta_tmp_path);
        return ERRCODE_FAIL;
    }

    LOG_INFO("DB-CONFIG: saved running configuration as '%s' (.db + .cfg + .meta)", resolved);
    ret = ERRCODE_SUCCESS;
    return ret;
}

// ============================================================================
// startup configuration
// ============================================================================

/**
 * @brief 校验新格式 cfg 快照的完整性元数据
 *
 * 旧快照可能没有 cfg_sha256/capture_complete，继续兼容；一旦元数据声明了这些
 * 字段，就必须完整且匹配，避免选择一个已截断或捕获失败的 cfg 作为启动源。
 */
static int db_config_validate_cfg_integrity(const char *name, char **err)
{
    char cfg_path[700];
    db_config_cfg_path(name, cfg_path, sizeof(cfg_path));

    gchar *cfg_text = NULL;
    gsize cfg_len = 0;
    if (!g_file_get_contents(cfg_path, &cfg_text, &cfg_len, NULL))
    {
        db_config_set_err(err, "Configuration cfg file is not readable");
        return ERRCODE_FAIL;
    }
    if (cfg_len == 0 || cfg_len > DB_CONFIG_CFG_MAX_BYTES)
    {
        g_free(cfg_text);
        db_config_set_err(err,
                          cfg_len == 0 ? "Configuration cfg file is empty" : "Configuration cfg file is too large");
        return ERRCODE_FAIL;
    }

    char meta_path[700];
    db_config_meta_path(name, meta_path, sizeof(meta_path));
    gchar *meta_text = NULL;
    if (!g_file_get_contents(meta_path, &meta_text, NULL, NULL))
    {
        /* pre-metadata legacy snapshot */
        g_free(cfg_text);
        return ERRCODE_SUCCESS;
    }

    char expected_sha[65] = "";
    char format[64] = "";
    gboolean capture_declared = FALSE;
    gboolean capture_complete = FALSE;
    gchar **lines = g_strsplit(meta_text, "\n", -1);
    for (guint i = 0; lines && lines[i]; i++)
    {
        char *line = g_strstrip(lines[i]);
        if (g_str_has_prefix(line, "cfg_sha256="))
        {
            g_strlcpy(expected_sha, g_strstrip(line + strlen("cfg_sha256=")), sizeof(expected_sha));
        }
        else if (g_str_has_prefix(line, "capture_complete="))
        {
            capture_declared = TRUE;
            capture_complete = strcmp(g_strstrip(line + strlen("capture_complete=")), "1") == 0;
        }
        else if (g_str_has_prefix(line, "format="))
        {
            g_strlcpy(format, g_strstrip(line + strlen("format=")), sizeof(format));
        }
    }
    g_strfreev(lines);
    g_free(meta_text);

    if (capture_declared && !capture_complete)
    {
        g_free(cfg_text);
        db_config_set_err(err, "Configuration capture is marked incomplete");
        return ERRCODE_FAIL;
    }
    if (format[0] != '\0' && strcmp(format, "bdr-indent-v1") != 0)
    {
        g_free(cfg_text);
        db_config_set_err(err, "Unsupported configuration cfg format");
        return ERRCODE_FAIL;
    }

    if (expected_sha[0] != '\0')
    {
        gchar *actual_sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)cfg_text, cfg_len);
        gboolean matches =
            actual_sha && strlen(expected_sha) == 64 && g_ascii_strcasecmp(actual_sha, expected_sha) == 0;
        g_free(actual_sha);
        if (!matches)
        {
            g_free(cfg_text);
            db_config_set_err(err, "Configuration cfg checksum mismatch");
            return ERRCODE_FAIL;
        }
    }

    g_free(cfg_text);
    return ERRCODE_SUCCESS;
}

int db_config_set_startup(const char *name, db_config_startup_mode_t mode, char **err)
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
    if (!db_config_startup_mode_valid(mode))
    {
        db_config_set_err(err, "Invalid startup mode (allowed: db, cfg)");
        return ERRCODE_FAIL;
    }

    char config_path[700];
    if (mode == DB_CONFIG_STARTUP_MODE_DB)
    {
        db_config_snapshot_path(name, config_path, sizeof(config_path));
    }
    else
    {
        db_config_cfg_path(name, config_path, sizeof(config_path));
    }

    if (access(config_path, R_OK) != 0)
    {
        db_config_set_err(err, "Configuration not found; use 'save configuration <name>' first");
        return ERRCODE_FAIL;
    }

    if (mode == DB_CONFIG_STARTUP_MODE_CFG && db_config_validate_cfg_integrity(name, err) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    if (db_config_write_startup_ptr(name, mode) != ERRCODE_SUCCESS)
    {
        db_config_set_err(err, "Failed to write startup pointer");
        return ERRCODE_FAIL;
    }

    LOG_INFO("DB-CONFIG: startup configuration set to '%s' (%s)", name, db_config_startup_mode_name(mode));
    return ERRCODE_SUCCESS;
}
