/**
 * @file   path_utils.c
 * @brief  路径工具函数实现
 * @author jhb
 * @date   2026/01/22
 */
#include "path_utils.h"

#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

int get_exe_dir(char *buf, size_t size)
{
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);

    if (len == -1)
    {
        return -1;
    }

    exe_path[len] = '\0';

    // Find the last '/' to get directory
    char *last_slash = strrchr(exe_path, '/');
    if (last_slash == NULL)
    {
        return -1;
    }

    *last_slash = '\0';

    if (strlen(exe_path) >= size)
    {
        return -1;
    }

    strncpy(buf, exe_path, size);
    return 0;
}

int resolve_xml_path(const char *module_name, char *buf, size_t size)
{
    struct stat st;
    char *path = NULL;

    /* 优先级 1: 环境变量 NN_WORK_DIR（生产环境） */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir != NULL)
    {
        path = g_build_filename(work_dir, "resources", module_name, "commands.xml", NULL);
        if (stat(path, &st) == 0)
        {
            strlcpy(buf, path, size);
            g_free(path);
            return 0;
        }
        g_free(path);
    }

    /* 优先级 2: 相对于可执行文件的开发路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        path = g_build_filename(exe_dir, "..", "..", "src", module_name, "resources", "commands.xml", NULL);
        if (stat(path, &st) == 0)
        {
            strlcpy(buf, path, size);
            g_free(path);
            return 0;
        }
        g_free(path);
    }

    LOG_WARN("Could not find XML file for module '%s'", module_name);
    return -1;
}

int file_read_first_line(const char *path, char *out, size_t out_size)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        return -1;
    }

    if (!fgets(out, (int)out_size, fp))
    {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    size_t n = strcspn(out, "\r\n");
    out[n] = '\0';
    return (out[0] != '\0') ? 0 : -1;
}

int resolve_version_file(char *path, size_t path_size)
{
    char *p = NULL;

    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir && work_dir[0] != '\0')
    {
        p = g_build_filename(work_dir, "VERSION", NULL);
        if (access(p, R_OK) == 0)
        {
            strlcpy(path, p, path_size);
            g_free(p);
            return 0;
        }
        g_free(p);
    }

    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        p = g_build_filename(exe_dir, "..", "..", "VERSION", NULL);
        if (access(p, R_OK) == 0)
        {
            strlcpy(path, p, path_size);
            g_free(p);
            return 0;
        }
        g_free(p);
    }

    strlcpy(path, "VERSION", path_size);
    if (access(path, R_OK) == 0)
    {
        return 0;
    }

    return -1;
}

int read_current_version(char *version, size_t version_size)
{
    char version_path[PATH_MAX];
    if (resolve_version_file(version_path, sizeof(version_path)) != 0)
    {
        return -1;
    }
    return file_read_first_line(version_path, version, version_size);
}
